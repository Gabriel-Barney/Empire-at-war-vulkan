// alamo_sim tests (no framework; non-zero exit on failure).
//   * pacing: speed table, floor(1000/rate) gating, last = now (no catch-up), pause / step, fast,
//     cinematic, wrap-around, stopped
//   * mode stack: galactic -> push space -> pop; only the top mode ticks, global frame continuous,
//     shared id counter, RNG not reseeded on push
//   * per-tick step order (T1..T13 and the tactical / common sub-steps)
//   * command handlers: select / move / attack / stop validation, wire id mapping, execute_direct
//   * synthetic space map (no game data): record a 600-tick run with live orders + AI stand-ins,
//     replay it at 0/1/4/N workers -> identical CRC logs, = the record run, = the checked-in golden
//     (testdata/synthetic_golden.crc; stream testdata/synthetic_stream.acmd must regenerate
//     byte-identically). Real time pacing gives the same CRCs as the fast runner.
//   * real data (ALAMO_FOC_DATA set; skipped otherwise): FoC space map _SPACE_PLANET_CORELLIA_PIRACY
//     and land map _LAND_PLANET_BYSS_KIDNAPPING: spawn with real stats, spatial queries = brute
//     force, 600 ticks replayed from testdata/<map>.acmd at 0/1/4/N workers = golden.
//   * S4 story (built with engine/lua): synthetic plot + Lua script over a synthetic galactic campaign,
//     fired-event log identical for 0/1/4/N workers = testdata/story_synthetic_golden.txt; with
//     ALAMO_FOC_DATA the opening of Full_Story_Campaign_Underworld = testdata/story_underworld_golden.txt.
//   * q6 game-object Lua scripts (SimConfig::object_scripts): attach order, the service gate and its first-pump
//     draw, ScriptExit / binding-error / runtime-error / death release, proximity order, a mini company spawner, the
//     save refusal, identical CRCs for 0/1/4 workers.
// `alamo_sim_tests --regen` rewrites the streams and goldens (MSVC is the reference compiler);
// `--regen-story` only the story logs; `--story-only` runs pacing + the synthetic story test.
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "alamo/commands/recording.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/scenario.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/story_model.hpp"  // dialog_log (L1/S4 wave 1 test)
#include "alamo/galactic/conflicts.hpp"  // BattleRequest (L1/S4 wave 2a test)
#include "alamo/lua/script_remaining.hpp"  // GlobalValueStore (q6 object-script test)
#endif

using namespace alamo;
using namespace alamo::sim;
namespace fs = std::filesystem;
namespace cmd = alamo::commands;

using ::alamo::test::g_fail;
static bool g_regen = false;

using ::alamo::test::env_or_empty;

static std::string testdata(const std::string& name) { return std::string(ALAMO_SIM_TESTDATA) + "/" + name; }

// ------------------------------------------------------------------------------------------ pacing
static void test_pacing() {
    FrameSynchronizer fs;
    CHECK(!fs.pace(0));  // stopped until a mode starts
    fs.reset();
    fs.set_stopped(false);
    CHECK(fs.rate() == 30 && fs.period_ms() == 33);
    CHECK(fs.pace(1000));
    CHECK(!fs.pace(1032));
    CHECK(fs.pace(1033));
    CHECK(fs.pace(2000));  // late: one tick, no catch-up
    CHECK(!fs.pace(2001) && !fs.pace(2032) && fs.pace(2033));
    fs.set_tactical_speed(4);
    fs.select_active(true);
    CHECK(fs.rate() == 60 && fs.period_ms() == 16);
    fs.set_galactic_speed(0);
    fs.select_active(false);
    CHECK(fs.rate() == 10 && fs.period_ms() == 100);
    fs.set_galactic_speed(99);
    CHECK(fs.active_speed_index() == 4 && fs.rate() == 60);
    fs.set_fast(true);
    CHECK(fs.rate() == 120 && fs.period_ms() == 8);
    fs.set_paused(true);
    CHECK(!fs.fast() && !fs.pace(5000));
    fs.request_step();
    CHECK(fs.pace(5001));
    CHECK(!fs.pace(9000));  // step consumed
    fs.set_paused(false);
    fs.set_cinematic_lock(true);
    CHECK(fs.rate() == 30);
    fs.set_fast(true);  // OI host: the cinematic lock wins over fast mode (MAIN_LOOP.md §1.2)
    CHECK(fs.rate() == 30);
    fs.reset();
    CHECK(fs.pace(0xFFFFFFF0u) && !fs.pace(0x10u) && fs.pace(0x11u));  // timeGetTime wrap
    fs.set_stopped(true);
    CHECK(!fs.pace(0x1000u));
}

// ---------------------------------------------------------------------------------- synthetic world
struct World {
    std::unique_ptr<DirectoryProvider> files;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
};

using ::alamo::test::write_file;

static std::unique_ptr<World> make_world() {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_tests_types";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>SimTest.xml</File></GameObjectFiles>\n");
    write_file(root / "Data/XML/SimTest.xml",
               "<SimTest>\n"
               " <SpaceUnit Name=\"Sim_Frigate\"><Victory_Relevant>Yes</Victory_Relevant>\n"
               "  <Tactical_Health>1200</Tactical_Health><Max_Speed>4.0</Max_Speed>\n"
               "  <Targeting_Max_Attack_Distance>900</Targeting_Max_Attack_Distance>\n"
               "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>\n"
               "  <Behavior>SELECTABLE, TARGETING</Behavior><SpaceBehavior>FIGHTER_LOCOMOTOR</SpaceBehavior>\n"
               " </SpaceUnit>\n"
               " <SpaceUnit Name=\"Sim_Corvette\"><Variant_Of_Existing_Type>Sim_Frigate</Variant_Of_Existing_Type>\n"
               "  <Max_Speed>6.5</Max_Speed><Tactical_Health>600</Tactical_Health></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sim_Station\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>8000</Tactical_Health>\n"
               "  <Custom_Hard_XExtent>300</Custom_Hard_XExtent><Behavior>SELECTABLE</Behavior>\n"
               "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sim_Asteroid\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>50</Tactical_Health><Scale_Factor>2.0</Scale_Factor>\n"
               "  <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sim_Planet\"><Behavior>PLANET</Behavior></SpaceUnit>\n"
               "</SimTest>\n");
    auto w = std::make_unique<World>();
    w->files = std::make_unique<DirectoryProvider>(path_to_utf8(root));
    CHECK(w->db.load(*w->files));
    CHECK(w->types.build(w->db));
    return w;
}

// Two fleets + a station each, neutral asteroids (owner 3), deterministic placement.
static terrain::MapData make_synthetic_map() {
    terrain::MapData m;
    uint32_t s = 12345u;
    auto rnd = [&s](float lo, float hi) {
        s = s * 1103515245u + 12345u;
        return lo + (hi - lo) * static_cast<float>((s >> 8) & 0xFFFFu) / 65535.0f;
    };
    auto add = [&m](const char* type, uint32_t owner, float x, float y, float yaw, float health) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        p.rotation_deg[2] = yaw;
        p.health_fraction = health;
        m.objects.push_back(p);
    };
    for (uint32_t side = 0; side < 2; ++side) {
        const float cx = side == 0 ? -3000.0f : 3000.0f;
        add("Sim_Station", side, cx * 1.3f, 0.0f, side ? 180.0f : 0.0f, 1.0f);
        for (int i = 0; i < 8; ++i) add("Sim_Frigate", side, cx + rnd(-600, 600), rnd(-1500, 1500), rnd(0, 360), 1.0f);
        for (int i = 0; i < 4; ++i) add("Sim_Corvette", side, cx + rnd(-400, 400), rnd(-1500, 1500), rnd(0, 360), 0.75f);
    }
    for (int i = 0; i < 30; ++i) add("SIM_ASTEROID", 3, rnd(-4500, 4500), rnd(-4500, 4500), rnd(0, 360), 1.0f);
    add("No_Such_Type", 3, 0, 0, 0, 1.0f);
    return m;
}

// ------------------------------------------------------------------------------------- mode stack
static void test_mode_stack(const World& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 777;
    SimHost host(w.types, js, cfg);
    CHECK(!host.tick_once());  // empty stack: simulation stopped
    GameMode& gal = host.start_game(ModeKind::Galactic);
    const auto planet = gal.primary().create({w.types.resolve_name("Sim_Planet")});
    CHECK(gal.primary().id(planet) == 1);
    host.random().synced().next15();  // galactic draws continue into the battle
    const uint32_t rng_before_push = host.random().synced().state();
    CHECK(host.run_fast(10) == 10 && gal.frame() == 10 && gal.ticks() == 10 && host.frame() == 10);
    GameMode& space = host.start_game(ModeKind::Space);
    CHECK(host.modes().depth() == 2 && host.modes().top() == &space && gal.suspended());
    CHECK(host.random().synced().state() == rng_before_push);  // not reseeded on push
    objects::SpawnParams sp;
    sp.type = w.types.resolve_name("Sim_Frigate");
    sp.parent_mode_id = 1;
    const ObjectHandle ship = host.tactical()->spawn(sp);
    CHECK(space.primary().id(ship) == 2);  // shared id counter
    CHECK(host.run_fast(20) == 20);
    CHECK(gal.frame() == 10 && gal.ticks() == 10);  // suspended mode does not tick
    CHECK(space.frame() == 20 && space.ticks() == 20 && host.frame() == 30);
    CHECK(host.tactical()->tactical_frame() == 20);
    // Parent-mode death marking reaches the galactic counterpart (type differs: container path).
    host.tactical()->destroy(ship);
    host.run_fast(1);
    CHECK(space.primary().live_count() == 0 && host.tactical()->trees().size() == 0);
    CHECK(host.end_game_mode());
    CHECK(host.modes().depth() == 1 && !gal.suspended());
    gal.set_paused(true);
    CHECK(host.run_fast(5) == 5 && gal.frame() == 10 && gal.ticks() == 15 && host.frame() == 36);
    gal.set_paused(false);
    CHECK(host.run_fast(5) == 5 && gal.frame() == 15);
    CHECK(host.crc_log().records.size() == 42 && host.crc_log().records.front().tick == 0 &&
          host.crc_log().records.back().tick == 41);
    CHECK(host.end_game_mode() && !host.tick_once() && !host.end_game_mode());
    // A new session restarts at frame 0 with the seed and a fresh id counter.
    GameMode& again = host.start_game(ModeKind::Land);
    CHECK(host.frame() == 0 && host.random().synced().state() == 777u &&
          again.primary().id(again.primary().create({w.types.resolve_name("Sim_Planet")})) == 1);
    CHECK(host.run_fast(3) == 3 && host.crc_log().records.size() == 4);
}

static void test_tick_order(const World& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimHost host(w.types, js);
    host.start_game(ModeKind::Land);
    std::vector<std::string> trace;
    host.set_trace(&trace);
    host.tick_once();
    const std::vector<std::string> expect = {
        "T1.begin_tick", "T2.commands",     "T4.treecull",  "gmm.scoring",          "tactical.begin",
        "tactical.ai_data", "tactical.motion_plan", "land.weather", "common.begin", "common.fow",
        "store.refit",   "common.store.primary", "common.store.secondary", "common.end", "tactical.end",
        "T5.gmm_update", "T7.players",      "T12.post_tick", "T13.end_tick"};
    CHECK(trace == expect);
    if (trace != expect)
        for (const auto& s : trace) std::printf("  trace: %s\n", s.c_str());
    host.end_game_mode();
    host.start_game(ModeKind::Galactic);
    trace.clear();
    host.tick_once();
    CHECK(trace.size() > 5 && trace[3] == "gmm.scoring" && trace[4] == "galactic.begin" && trace[5] == "common.begin");
}

// --------------------------------------------------------------------------------------- commands
static void check_spatial(const TacticalMode& tm);

static void test_commands(const World& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimHost host(w.types, js);
    host.start_game(ModeKind::Space);
    const TypeCrcIndex index(w.types);
    const terrain::MapData map = make_synthetic_map();
    const SpawnReport r = spawn_map(*host.tactical(), map, index);
    CHECK(r.placements == 57 && r.spawned == 56 && r.unknown_type == 1 && r.units == 24 && r.structures == 2 &&
          r.props == 30 && r.in_trees == 56);
    CHECK(r.unknown_crcs.size() == 1 && r.unknown_crcs[0].first == name_crc("No_Such_Type"));
    TacticalMode& tm = *host.tactical();
    objects::ObjectStore& st = tm.primary();
    check_spatial(tm);
    // Real stats: corvette health = 0.75 x 600 (variant overrides the parent's 1200).
    const ObjectHandle station0 = st.find(1), frig0 = st.find(2), corv0 = st.find(10), station1 = st.find(14);
    CHECK(st.health(station0) == 8000.0f && st.health(frig0) == 1200.0f && st.health(corv0) == 450.0f);
    CHECK(st.owner(station1) == 1 && tm.movable(frig0) && !tm.movable(station0));
    CHECK(host.wire_id(frig0) == 2 && host.resolve(2) == frig0 && !host.resolve(cmd::kNoObject) && !host.resolve(999));
    // Select own frigate + a foreign one: only the own unit stays selected.
    CHECK(host.submit(0, cmd::ev::select({2, 15})).ok());
    CHECK(host.submit(0, cmd::ev::move_to_position(-1000.0f, 500.0f, 0.0f)).ok());
    CHECK(host.submit(1, cmd::ev::attack(2, {15, 16})).ok());   // player 1 attacks player 0's frigate
    CHECK(host.submit(1, cmd::ev::attack(15, {16})).ok());      // own target: rejected
    CHECK(host.submit(0, cmd::ev::move_to_position(9000.0f, 0.0f, 0.0f)).ok());  // out of bounds
    CHECK(host.tactical()->order(2) == nullptr);
    // SP delay 1: before the first drain the scheduler's now is tick 1, so these apply at tick 2.
    host.tick_once();
    CHECK(host.commands_applied() == 0 && tm.order(2) == nullptr);
    host.tick_once();
    CHECK(host.selection(0) == std::vector<ObjectId>{2});
    const UnitOrder* mv = tm.order(2);
    CHECK(mv && mv->kind == OrderKind::Move && mv->issued_frame == 2 && mv->goal.x == cmd::requantize(1, -1000.0f) &&
          mv->goal.y == cmd::requantize(2, 500.0f));
    const UnitOrder* at = tm.order(15);
    CHECK(at && at->kind == OrderKind::Attack && at->target == 2 && tm.order(16) && tm.order(16)->player == 1);
    CHECK(host.commands_applied() == 3 && host.commands_rejected() == 2);
    // The stub locomotor moved the frigate by Max_Speed toward the goal during tick 2's service.
    const Vec3 p0{map.objects[1].position[0], map.objects[1].position[1], 0.0f};
    const Vec3 p1 = st.position(frig0);
    const float moved = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
    CHECK(std::fabs(moved - 4.0f) < 1e-3f);
    // Stop, and an AI order through execute_direct (applies at once, not recorded).
    CHECK(host.submit(0, cmd::ev::stop()).ok());
    host.tick_once();
    CHECK(tm.order(2) == nullptr);
    CHECK(host.execute_direct(0, cmd::CommandSource::AI, cmd::ev::attack(15, {3})) == cmd::RejectReason::None);
    CHECK(tm.order(3) && tm.order(3)->kind == OrderKind::Attack && tm.order(3)->issued_frame == 3);
    CHECK(host.execute_direct(0, cmd::CommandSource::AI, cmd::ev::attack(15, {20})) == cmd::RejectReason::InvalidUnit);
    // Destroyed target: the attack order is dropped silently at the next tick.
    tm.destroy(st.find(15));
    host.tick_once();
    CHECK(tm.order(3) == nullptr && tm.trees().size() == 55 && !host.resolve(15));
}

// bkc1 t04 (backlog b0012; COMMANDS §2.4.1, q6 t15 probe 6): SelectAll keeps the issuer's SELECTABLE units and leaves out
// base structures (DUMMY_GROUND_STRUCTURE: barracks, factories), star bases (DUMMY_STAR_BASE: the own space station),
// orbital structures and mines, plus anything not SELECTABLE (build pads have no SELECTABLE in the shipped data: the
// FoC check in alamo_objects_tests); other players' units are never selected; the selection is replaced.
static void test_select_all() {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_tests_select_all";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>SelTest.xml</File></GameObjectFiles>\n");
    write_file(root / "Data/XML/SelTest.xml",
               "<SelTest>\n"
               " <SpaceUnit Name=\"Sel_Ship\"><Tactical_Health>100</Tactical_Health><Max_Speed>4.0</Max_Speed>\n"
               "  <Behavior>SELECTABLE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sel_Station\"><Tactical_Health>100</Tactical_Health><Behavior>SELECTABLE, DUMMY_STAR_BASE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sel_Barracks\"><Tactical_Health>100</Tactical_Health><Behavior>SELECTABLE, DUMMY_GROUND_STRUCTURE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sel_Orbital\"><Tactical_Health>100</Tactical_Health><Behavior>SELECTABLE, DUMMY_ORBITAL_STRUCTURE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sel_Mine\"><Tactical_Health>100</Tactical_Health><Behavior>SELECTABLE, PROXIMITY_MINE</Behavior></SpaceUnit>\n"
               " <SpaceUnit Name=\"Sel_Pad\"><Tactical_Health>100</Tactical_Health><Behavior>REVEAL</Behavior></SpaceUnit>\n"
               "</SelTest>\n");
    DirectoryProvider files(path_to_utf8(root));
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
    CHECK(db.load(files));
    CHECK(types.build(db));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimHost host(types, js);
    host.start_game(ModeKind::Space);
    auto put = [&](const char* type, int32_t owner, float x) {
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(db.find(type));
        sp.owner = owner;
        sp.position = {x, 0.0f, 0.0f};
        sp.facing = {1.0f, 0.0f, 0.0f};
        const ObjectHandle h = host.tactical()->spawn(sp);
        CHECK(host.tactical()->primary().alive(h));
        return host.tactical()->primary().id(h);
    };
    const ObjectId ship_a = put("Sel_Ship", 0, 0.0f);
    put("Sel_Station", 0, 100.0f);
    put("Sel_Barracks", 0, 200.0f);
    put("Sel_Orbital", 0, 300.0f);
    put("Sel_Mine", 0, 400.0f);
    put("Sel_Pad", 0, 500.0f);
    const ObjectId ship_b = put("Sel_Ship", 0, 600.0f);
    const ObjectId enemy = put("Sel_Ship", 1, 700.0f);
    CHECK(host.submit(0, cmd::ev::select({static_cast<cmd::ObjectId>(enemy)})).ok());
    CHECK(host.submit(0, cmd::ev::select_all()).ok());
    host.tick_once();
    host.tick_once();
    CHECK(host.selection(0) == (std::vector<ObjectId>{ship_a, ship_b}));
    CHECK(host.submit(1, cmd::ev::select_all()).ok());
    host.tick_once();
    host.tick_once();
    CHECK(host.selection(1) == std::vector<ObjectId>{enemy});
    remove_all_long(root, ec);
}

// Spatial: right after the map load the trees answer = brute force over the store.
static void check_spatial(const TacticalMode& tm) {
    const objects::ObjectStore& st = tm.primary();
    tactical::QueryContext ctx;
    tactical::Sphere sph{{-3000.0f, 0.0f, 0.0f}, 1200.0f};
    std::vector<tactical::EntityHandle> brute;
    for (ObjectHandle h : st.all_objects())
        if (st.alive(h) && tactical::overlaps(sph, tm.bounds_of(h))) brute.push_back(static_cast<uint32_t>(st.id(h)));
    std::sort(brute.begin(), brute.end());
    const auto got = tm.trees().all().query_sphere(sph, tactical::kSkipDead, ctx);
    CHECK(std::vector<tactical::EntityHandle>(got.begin(), got.end()) == brute && brute.size() > 5);
}

// --------------------------------------------------------------------------- determinism + golden
static bool same_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> have;
    return read_host_file(path, have) && have == bytes;
}

// Records a live run of `map`, replays it at every worker count; compares with the stored stream
// and golden (or rewrites them with --regen).
static void determinism(const objects::GameObjectTypes& types, const terrain::MapData& map,
                        const std::string& stem, uint64_t ticks) {
    const TypeCrcIndex index(types);
    cmd::CommandRecording rec;
    MapRunSpec spec;
    spec.ticks = ticks;
    spec.seed = 4242;
    spec.workers = 0;
    spec.record = &rec;
    const MapRunResult live = run_map(types, map, index, spec);
    rec.meta = {{"map", stem}};
    std::printf("%s: %" PRIu64 " ticks, %u spawned (%u units), %" PRIu64 " live orders, %zu recorded commands, applied %" PRIu64
                " rejected %" PRIu64 ", orders pending %u, %.1f ms, final CRC %016" PRIx64 " parity %08x\n",
                stem.c_str(), live.ticks, live.spawn.spawned, live.spawn.units, live.orders_live, rec.command_count(),
                live.applied, live.rejected, live.orders_pending, live.run_ms, live.log.records.back().combined,
                live.log.records.back().parity);
    CHECK(live.ticks == ticks && live.log.records.size() == ticks + 1 && rec.command_count() > 50);
    CHECK(live.applied > 20 && live.rejected > 5);
    const std::string stream_path = testdata(stem + ".acmd"), golden_path = testdata(stem + "_golden.crc");
    const std::vector<uint8_t> bytes = rec.to_bytes();
    if (g_regen) {
        std::string err;
        CHECK(rec.save(stream_path, &err));
    } else {
        CHECK(same_file(stream_path, bytes));  // the record path regenerates the stream byte for byte
    }
    cmd::CommandRecording stored;
    std::string err;
    if (!cmd::CommandRecording::load(stream_path, stored, &err)) {
        std::printf("FAIL %s: %s\n", stream_path.c_str(), err.c_str());
        ++g_fail;
        return;
    }
    CrcLog first;
    for (int32_t workers : jobs::determinism_worker_counts()) {
        MapRunSpec r;
        r.ticks = ticks;
        r.workers = workers;
        r.replay = &stored;
        const MapRunResult out = run_map(types, map, index, r);
        const auto d = first_divergence(live.log, out.log);
        std::printf("  replay workers %2d: %.1f ms, %s\n", workers, out.run_ms, d.message.c_str());
        CHECK(!d.diverged && out.applied == live.applied && out.rejected == live.rejected);
        if (first.records.empty()) first = out.log;
    }
    first.comments = {"G0 " + stem + " replay, seed 4242, " + std::to_string(ticks) + " ticks"};
    if (g_regen) {
        CHECK(first.save(golden_path, &err));
        std::printf("  regenerated %s and %s\n", stream_path.c_str(), golden_path.c_str());
    } else {
        CrcLog golden;
        if (!CrcLog::load(golden_path, golden, &err)) {
            std::printf("FAIL golden %s: %s\n", golden_path.c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto d = first_divergence(golden, first);
            std::printf("  golden %s: %s\n", (stem + "_golden.crc").c_str(), d.message.c_str());
            CHECK(!d.diverged);
        }
    }
}

// --- N1 (owner: n1-host): session descriptor, save_allowed, load refusals and a host-chunk round trip (galactic mode
// without a GalacticSim; the data-driven sessions are alamo_headless --save-check) ---
static void test_save_session(const World& w) {
    // Descriptor: round trip, compared fields, informational fields, truncated data.
    SessionDescriptor d;
    d.kind = "galactic";
    d.campaign = "Save_Test";
    d.data_root = "C:/game";
    d.data_args = {"MODPATH=Mods/X"};
    d.seed = 3;
    d.players = 4;
    d.setup = {{"Empire", true, false, "", ""}, {"Rebel", false, true, "Basic_AI", "Normal"}};
    d.difficulty = "Normal";
    d.set_option("b", "2");
    d.set_option("a", "1");
    d.data_fingerprint = 0x1234;
    d.writer = "test";
    ByteWriter dw;
    d.write(dw);
    SessionDescriptor e;
    ByteReader dr(dw.bytes());
    CHECK(e.read(dr) && dr.done());
    CHECK(SessionDescriptor::mismatch(d, e).empty() && e.option("a") == "1" && e.option("zz", "x") == "x");
    SessionDescriptor f = e;
    f.data_root = "D:/elsewhere";
    f.writer = "other";
    f.options = {{"a", "1"}, {"b", "2"}};  // any order
    CHECK(SessionDescriptor::mismatch(d, f).empty());
    f = e;
    f.data_fingerprint = 0x9999;
    CHECK(SessionDescriptor::mismatch(d, f).find("fingerprint") != std::string::npos);
    f = e;
    f.seed = 4;
    CHECK(!SessionDescriptor::mismatch(d, f).empty());
    f = e;
    f.setup[1].ai_type = "Other_AI";
    CHECK(!SessionDescriptor::mismatch(d, f).empty());
    f = e;
    f.set_option("a", "2");
    CHECK(!SessionDescriptor::mismatch(d, f).empty());
    SessionDescriptor g;
    g.kind = "keep";
    ByteReader shortr(std::span<const uint8_t>(dw.bytes()).first(dw.size() - 3));
    CHECK(!g.read(shortr) && g.kind == "keep");

    // A host: galactic mode with two objects, a controller drawing the synced RNG, a participant with its own state.
    struct Counter {
        uint32_t value = 0;
    };
    struct Run {
        std::unique_ptr<jobs::JobSystem> js;
        Counter counter;
        std::unique_ptr<SimHost> host;
    };
    auto build = [&w](uint32_t seed) {
        auto r = std::make_unique<Run>();
        r->js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{0});
        SimConfig cfg;
        cfg.seed = seed;
        r->host = std::make_unique<SimHost>(w.types, *r->js, cfg);
        GameMode& gal = r->host->start_game(ModeKind::Galactic);
        (void)gal.primary().create({w.types.resolve_name("Sim_Planet")});
        (void)gal.primary().create({w.types.resolve_name("Sim_Station")});
        Counter* c = &r->counter;
        r->host->set_controller(0, [c](SimHost& h, uint8_t, uint64_t frame) {
            h.random().synced().next15();
            c->value += static_cast<uint32_t>(frame % 7u);
        });
        SaveParticipant p;
        p.name = "test.counter";
        p.write = [c](ByteWriter& out) { out.u32(c->value); };
        p.read = [c](ByteReader& in) {
            const uint32_t v = in.u32();
            if (!in.ok()) return false;
            c->value = v;
            return true;
        };
        r->host->add_save_participant(std::move(p));
        SessionDescriptor sd;
        sd.kind = "test";
        sd.campaign = "Save_Test";
        r->host->set_session(sd);
        return r;
    };
    auto a = build(11);
    bool threw = false;
    try {
        a->host->add_save_participant({"test.counter", 1, [](ByteWriter&) {}, [](ByteReader&) { return true; }, {}, {}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);  // duplicate name
    threw = false;
    try {
        a->host->add_save_participant({"rng", 1, [](ByteWriter&) {}, [](ByteReader&) { return true; }, {}, {}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);  // host chunk name
    CHECK(a->host->run_fast(40) == 40 && a->host->save_allowed());
    // A tactical mode on the stack blocks saving when no participant can rebuild it at load (stage 2; see below).
    a->host->start_game(ModeKind::Space);
    std::string why;
    SaveFile blocked;
    CHECK(!a->host->save_allowed(&why) && !why.empty() && !a->host->save(blocked));
    CHECK(a->host->end_game_mode() && a->host->save_allowed());
    CHECK(a->host->run_fast(20) == 20);
    SaveFile save;
    std::string err;
    CHECK(a->host->save(save, &err) && save.tick == 60 && save.find("session") && save.find("test.counter"));
    for (const char* c : {"frame", "objects", "galactic.state", "players", "rng", "commands", "globals"}) CHECK(save.find(c) != nullptr);
    const uint32_t counter_at_save = a->counter.value;
    CHECK(a->host->run_fast(60) == 60);
    CrcLog log_a;
    for (const CrcRecord& r : a->host->crc_log().records)
        if (r.tick >= 60) log_a.records.push_back(r);
    log_a.systems = a->host->crc_log().systems;
    log_a.parity_parts = a->host->crc_log().parity_parts;

    // Load into a fresh session: every chunk loads, the run continues with the original's CRCs.
    auto b = build(11);
    LoadReport rep;
    CHECK(b->host->load(save, &err, &rep));
    if (!rep.ok()) std::printf("  save test load: %s\n", rep.text().c_str());
    CHECK(rep.failed.empty() && rep.missing.empty() && rep.unexpected.empty() && b->counter.value == counter_at_save);
    CHECK(b->host->frame() == 60 && b->host->run_fast(60) == 60);
    const CrcDivergence dv = first_divergence(log_a, b->host->crc_log());
    if (dv.diverged) std::printf("  save test: %s\n", dv.message.c_str());
    CHECK(!dv.diverged && b->counter.value == a->counter.value);
    CHECK(!b->host->load(save, &err));  // not a fresh session any more

    // Refusals: another seed (descriptor), a broken chunk (named), a missing chunk (named).
    auto c = build(12);
    CHECK(!c->host->load(save, &err, &rep) && !rep.descriptor_error.empty());
    SaveFile broken, partial;
    broken.tick = partial.tick = save.tick;
    for (const SaveChunk& ch : save.chunks()) {
        std::vector<uint8_t> bytes = ch.data;
        if (ch.name == "players") bytes.resize(bytes.size() - 1);
        broken.add(ch.name, ch.version, std::move(bytes));
        if (ch.name != "rng") partial.add(ch.name, ch.version, ch.data);
    }
    auto d1 = build(11);
    CHECK(!d1->host->load(broken, &err, &rep) && rep.failed == std::vector<std::string>{"players"} &&
          err.find("players") != std::string::npos);
    auto d2 = build(11);
    CHECK(!d2->host->load(partial, &err, &rep) && rep.missing == std::vector<std::string>{"rng"});
    std::printf("save session: descriptor, save_allowed, %zu chunks, load + %" PRIu64 " ticks %s, refusals checked\n", save.chunks().size(),
                uint64_t{60}, dv.diverged ? "DIVERGED" : "identical");

    // N1 stage 2 (n2-host): with a participant that can rebuild it, a tactical mode on the stack saves; the load runs the
    // two passes (pass 1 stages, rebuild pushes the mode, pass 2 reads everything) and the run continues identically.
    struct Glue {
        SimHost* host = nullptr;
        bool resume = false;
        int reads = 0, rebuilds = 0;
    };
    auto add_glue = [](SimHost& h) {
        auto g = std::make_shared<Glue>();
        g->host = &h;
        SaveParticipant p;
        p.name = "test.battle";
        p.write = [g](ByteWriter& w) { w.boolean(g->host->modes().depth() > 1); };
        p.read = [g](ByteReader& r) {
            const bool tactical = r.boolean();
            if (!r.ok()) return false;
            ++g->reads;
            g->resume = tactical && g->host->modes().depth() == 1;
            return true;
        };
        p.rebuild = [g] {
            if (!g->resume) return std::string();
            g->resume = false;
            ++g->rebuilds;
            g->host->start_game(ModeKind::Space);
            return std::string();
        };
        h.add_save_participant(std::move(p));
        return g;
    };
    auto ts = build(11);
    add_glue(*ts->host);
    CHECK(ts->host->run_fast(30) == 30);
    ts->host->start_game(ModeKind::Space);
    CHECK(ts->host->run_fast(10) == 10 && ts->host->save_allowed());
    SaveFile tsave;
    CHECK(ts->host->save(tsave, &err) && tsave.find("spatial") != nullptr);
    CHECK(ts->host->run_fast(30) == 30);
    CrcLog log_e;
    for (const CrcRecord& r : ts->host->crc_log().records)
        if (r.tick >= tsave.tick) log_e.records.push_back(r);
    log_e.systems = ts->host->crc_log().systems;
    log_e.parity_parts = ts->host->crc_log().parity_parts;
    auto tl = build(11);
    const auto fg = add_glue(*tl->host);
    CHECK(tl->host->load(tsave, &err, &rep));
    if (!rep.ok()) std::printf("  tactical save test load: %s\n", rep.text().c_str());
    CHECK(tl->host->modes().depth() == 2 && fg->rebuilds == 1 && fg->reads == 2 && tl->host->frame() == tsave.tick);
    CHECK(tl->host->run_fast(30) == 30);
    const CrcDivergence tdv = first_divergence(log_e, tl->host->crc_log());
    if (tdv.diverged) std::printf("  tactical save test: %s\n", tdv.message.c_str());
    CHECK(!tdv.diverged);
    // Without a participant that rebuilds: the load stops after pass 1 with a named reason.
    auto g2 = build(11);
    CHECK(!g2->host->load(tsave, &err, &rep) && !rep.descriptor_error.empty());
    std::printf("save session (stage 2): tactical mode saved at %" PRIu64 ", rebuilt on load, 30 ticks %s; no rebuilder refused (%s)\n",
                tsave.tick, tdv.diverged ? "DIVERGED" : "identical", rep.descriptor_error.c_str());
}

static void test_realtime_equals_fast(const World& w) {
    const TypeCrcIndex index(w.types);
    const terrain::MapData map = make_synthetic_map();
    MapRunSpec fast;
    fast.ticks = 45;
    fast.workers = 0;
    const MapRunResult a = run_map(w.types, map, index, fast);
    MapRunSpec paced = fast;
    paced.realtime = true;
    paced.workers = 2;
    const MapRunResult b = run_map(w.types, map, index, paced);
    CHECK(!first_divergence(a.log, b.log).diverged);
    // 45 ticks at 30 ticks/s with floor(1000/30) = 33 ms gating: at least 44 x 33 ms of wall time.
    CHECK(b.run_ms >= 44.0 * 33.0 - 1.0);
    std::printf("realtime: 45 ticks in %.0f ms (paced), %.2f ms (fast), identical CRCs\n", b.run_ms, a.run_ms);
}

// ------------------------------------------------------------------------------------ S4 story
static bool g_regen_story = false;
static bool g_story_only = false;

static std::string read_text(const std::string& path) {
    std::vector<uint8_t> b;
    if (!read_host_file(path, b)) return {};
    std::string s(b.begin(), b.end());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Compares `text` with testdata/<name> (or rewrites it with --regen / --regen-story).
static void check_text_golden(const std::string& name, const std::string& text) {
    const std::string path = testdata(name);
    if (g_regen_story) {
        std::ofstream(path_from_utf8(path), std::ios::binary) << text;
        std::printf("  regenerated %s\n", name.c_str());
        return;
    }
    const std::string want = read_text(path);
    if (want == text) {
        std::printf("  golden %s: MATCH (%zu bytes)\n", name.c_str(), text.size());
        return;
    }
    size_t i = 0;
    while (i < want.size() && i < text.size() && want[i] == text[i]) ++i;
    const size_t line = static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(i), '\n')) + 1;
    std::printf("FAIL golden %s differs at line %zu\n", name.c_str(), line);
    ++g_fail;
}

#ifdef ALAMO_SIM_HAVE_STORY
namespace story_test {
using namespace alamo::galactic;
enum : uint32_t { tPA, tPB, tPC, tPD, tSB1, tCorvette, tInfantry, tBarracks, tFighters, tCount };
enum : PlanetIndex { rCore = 0, rA = 1, rB = 2, rC = 3, rD = 4 };
enum : int32_t { pEmpire = 0, pUnder = 1, pNeutral = 2 };

static GalacticRules rules() {
    GalacticRules r;
    FactionRules emp;
    emp.name = "Empire";
    emp.basic_ai = "BasicEmpire";
    emp.can_control_planets = emp.is_playable = true;
    FactionRules und;
    und.name = "Underworld";
    und.basic_ai = "AI_Player_Underworld";
    und.can_control_planets = und.is_playable = und.benefits_from_corruption = true;
    FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, und, neu};
    r.difficulties = {{"Normal", 1.0f, 1.0f}};
    r.types.resize(tCount);
    const char* names[] = {"PlanetA", "PlanetB", "PlanetC", "PlanetD"};
    for (uint32_t i = 0; i < 4; ++i) {
        r.types[i].name = names[i];
        r.types[i].kind = UnitKind::Planet;
        r.types[i].credit_value = 10;
        r.types[i].capture_bonus = 100;
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
    TypeInfo& co = unit(tCorvette, "Corvette", UnitKind::SpaceUnit, 800, 10);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 2.0f;
    co.ai_combat_power = 600.0f;
    co.squadron_capacity = 2;
    unit(tInfantry, "Infantry", UnitKind::GroundUnit, 300, 10).required_ground_base = 1;
    unit(tBarracks, "Barracks", UnitKind::GroundStructure, 600, 20);
    unit(tFighters, "Fighters", UnitKind::Squadron, 200, 2).required_star_base = 1;
    return r;
}

static GalacticSetup setup() {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "StoryTest";
    const char* names[] = {"Core", "A", "B", "C", "D"};
    const Vec3 pos[] = {{500, 500, 0}, {0, 0, 0}, {60, 0, 0}, {120, 0, 0}, {60, 80, 0}};
    for (uint32_t i = 0; i < 5; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = pos[i];
        m.planets.flags[i] = i == 0 ? 0u : static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b) {
        const uint32_t r = m.routes.add(n);
        m.routes.point_a[r] = a;
        m.routes.point_b[r] = b;
        m.routes.hs_speed_factor[r] = 8.0f;
    };
    route("A_B", rA, rB);
    route("B_C", rB, rC);
    route("B_D", rB, rD);
    s.planet_type = {kNoIndex, tPA, tPB, tPC, tPD};
    PlayerSetup e;
    e.faction = 0;
    e.human = true;
    e.credits = 5000.0f;
    PlayerSetup u;
    u.faction = 1;
    u.ai = true;
    u.difficulty = 0;
    u.credits = 5000.0f;
    PlayerSetup n;
    n.faction = 2;
    s.players = {e, u, n};
    s.forces = {{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rD, tCorvette}, {pUnder, rD, tInfantry}};
    return s;
}

// Plots file + plot (every mapped trigger kind, a reward the host applies, a Lua round trip).
static const char* kPlots =
    "<Story_Mode_Plots><Lua_Script>Story_Host_Test</Lua_Script><Active_Plot>Story_Host_Test.xml</Active_Plot>"
    "<Suspended_Plot /></Story_Mode_Plots>\n";
static const char* kPlot =
    "<Story>\n"
    " <Event Name=\"Begin\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n"
    " <Event Name=\"Raise_Tech\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_TECH_LEVEL</Reward_Type>"
    "<Reward_Param1>Empire</Reward_Param1><Reward_Param2>3</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Tech_Seen\"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>3</Event_Param1>"
    "<Prereq>Raise_Tech</Prereq></Event>\n"
    " <Event Name=\"Script_Ack\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>SCRIPT_SAW_TECH</Event_Param2>"
    "<Reward_Type>SET_FLAG</Reward_Type><Reward_Param1>Acked</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>\n"
    " <Event Name=\"Enemy_Takes_C\"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3><Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Friendly_Takes_C\"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param3>FILTER_FRIENDLY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Corvette_At_C\"><Event_Type>STORY_ENTER</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param3>Corvette</Event_Param3></Event>\n"
    " <Event Name=\"Landed_C\"><Event_Type>STORY_LAND_ON</Event_Type><Event_Param1>PlanetC</Event_Param1></Event>\n"
    " <Event Name=\"C_Corrupted\"><Event_Type>STORY_CORRUPTION_CHANGED</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param2>RACKETEERING</Event_Param2></Event>\n"
    " <Event Name=\"Two_Fighters\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Fighters</Event_Param1>"
    "<Event_Param2>2</Event_Param2><Event_Param3>FILTER_FRIENDLY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Bonus\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>CREDITS</Reward_Type>"
    "<Reward_Param1>1000</Reward_Param1><Prereq>Two_Fighters</Prereq></Event>\n"
    " <Event Name=\"Two_Seconds\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>2</Event_Param1>"
    "<Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Ack_Flag\"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>Acked</Event_Param1>"
    "<Event_Param2>1</Event_Param2></Event>\n"
    "</Story>\n";
static const char* kScript =
    "function Base_Definitions() ServiceRate = 0.5 services = 0 end\n"
    "function Story_Event_Trigger(name) if name == \"Tech_Seen\" then Story_Event(\"SCRIPT_SAW_TECH\") end end\n"
    "function main() while true do services = services + 1 coroutine.yield(true) end end\n";

struct Run {
    std::string log;
    CrcLog crc;
    size_t fired = 0;
    float empire_credits = 0;
    int32_t empire_tech = 0;
    // q6 t09 (scoring runs): the script's GlobalValue Scoring_Log, its messages and the host counters.
    std::string scoring_log;
    std::vector<std::string> scoring_messages;
    uint64_t scoring_services = 0, scoring_calls = 0, scoring_releases = 0;
};

// scoring: the GameScoring script name to run (q6 t09; nullptr = none, the S4 golden run).
static Run run(const World& w, const FileProvider& files, int32_t workers, uint64_t ticks, const char* scoring = nullptr) {
    const GalacticRules r = rules();
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 7;
    cfg.story_crc = true;
    SimHost host(w.types, js, cfg);
    if (scoring) {
        host.set_scoring_script(&files, scoring);
        host.set_scoring_campaign("StoryTest");
    }
    GalacticSim* gs = host.start_galactic(r, setup());
    CHECK(gs != nullptr);
    if (!gs) return {};
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{pEmpire, "Story_Plots_Host_Test.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& d : diag) std::printf("  story diag: %s\n", d.c_str());
    if (!host.story()) return {};
    CHECK(host.story()->script_count() == 1);
    std::vector<std::string> trace;
    host.set_trace(&trace);
    host.tick_once();
    CHECK(std::find(trace.begin(), trace.end(), "T9.story") != trace.end());
    host.set_trace(nullptr);
    // Scripted orders between ticks (state-driven, deterministic): Underworld loads the infantry, flies
    // D -> B -> C and lands; the Empire builds two fighters at A; a space battle is pushed and popped.
    const ObjectHandle inf = gs->planets()[rD].surface[0];
    CHECK(gs->launch_units(pUnder, rD, std::vector<ObjectHandle>{inf}) == RejectReason::None);
    const uint32_t fleet = gs->fleets_at(rD, pUnder)[0];
    CHECK(gs->move_fleet(pUnder, fleet, rC) == RejectReason::None);
    CHECK(gs->enqueue(pEmpire, rA, tFighters) == RejectReason::None);
    CHECK(gs->enqueue(pEmpire, rA, tFighters) == RejectReason::None);
    bool landed = false;
    for (uint64_t t = 1; t < ticks; ++t) {
        if (!landed && gs->fleets()[fleet].alive && !gs->fleets()[fleet].moving && gs->fleets()[fleet].planet == rC) {
            CHECK(gs->invade(pUnder, rC, {}) == RejectReason::None);
            landed = true;
        }
        if (t == 200) {
            host.start_game(ModeKind::Space);
            host.scoring_mode_started(ModeKind::Space, "Test_Space_Map");  // q6 t09 (no-op without a script)
        }
        if (t == 230) host.end_game_mode();
        host.tick_once();
    }
    Run out;
    out.scoring_log = host.global_values()->get("Scoring_Log").text;
    out.scoring_messages = host.scoring_messages();
    out.scoring_services = host.scoring_services();
    out.scoring_calls = host.scoring_calls();
    out.scoring_releases = host.scoring_releases();
    out.log = host.story()->log_text("S4 synthetic story campaign, seed 7, " + std::to_string(ticks) + " ticks");
    out.crc = host.crc_log();
    out.fired = host.story()->log().size();
    out.empire_credits = gs->credits(pEmpire);
    out.empire_tech = gs->players()[pEmpire].tech_level;
    for (const auto& m : host.story()->lua_messages()) std::printf("  lua: %s\n", m.c_str());
    CHECK(host.story()->lua_messages().empty());
    return out;
}

// Q6: a campaign with more players than the default session (Thrawn's Revenge: 22). The host refuses it at the default
// size (those players would get no controller), starts it sized with session_players, and every player has a controller
// slot; <Story_Name> "Faction, file" pairs give plot sets to factions without a <Faction>_Story_Name.
static void test_many_players(const World& w) {
    CHECK(session_players(3) == 8 && session_players(22) == 22 && session_players(100) == kMaxPlayers);
    GalacticRules r = rules();
    GalacticSetup s = setup();
    for (uint32_t i = 0; i < 17; ++i) {  // 3 + 17 = 20 players: Mod_0..Mod_16 (planet-less AI factions)
        FactionRules f;
        f.name = "Mod_" + std::to_string(i);
        f.can_control_planets = f.is_playable = true;
        r.factions.push_back(f);
        PlayerSetup p;
        p.faction = static_cast<uint32_t>(r.factions.size() - 1);
        p.ai = true;
        p.credits = 1000.0f;
        s.players.push_back(p);
    }
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    {
        SimHost host(w.types, js, SimConfig{});
        std::vector<std::string> diag;
        CHECK(host.start_galactic(r, s, &diag) == nullptr);
        CHECK(!diag.empty() && diag.front().find("20 players") != std::string::npos);
    }
    SimConfig cfg;
    cfg.players = session_players(s.players.size());
    SimHost host(w.types, js, cfg);
    GalacticSim* gs = host.start_galactic(r, s);
    CHECK(gs != nullptr && host.players() == 20);
    if (!gs) return;
    uint64_t last_frame = 0;
    host.set_controller(19, [&last_frame](SimHost&, uint8_t, uint64_t frame) { last_frame = frame; });
    host.run_fast(5);
    CHECK(last_frame > 0);  // the last player's controller runs

    data::CampaignDef c;
    c.raw = {{"Empire_Story_Name", "Story_Plots_E.xml"},
             {"Story_Name", " Empire, Second_E.xml, Mod_3 , Sub\\Story_Plots_M3.xml,\n Mod_16, M16.xml, Dangling "},
             {"Story_Name", "Mod_3, Later.xml, Mod_5, M5.xml"}};
    const std::vector<StoryPlotSet> sets = campaign_plot_sets(c, *gs);
    // q6 t03 (STORY.md §2.1 [H]): the faction tags first, then every Story_Name pair in list order (a faction may get
    // several sets); a dangling name is dropped.
    CHECK(sets.size() == 6);
    if (sets.size() == 6) {
        CHECK(sets[0].player == pEmpire && sets[0].plots_file == "Story_Plots_E.xml");
        CHECK(sets[1].player == pEmpire && sets[1].plots_file == "Second_E.xml");
        CHECK(sets[2].player == 3 + 3 && sets[2].plots_file == "Sub\\Story_Plots_M3.xml");
        CHECK(sets[3].player == 3 + 16 && sets[3].plots_file == "M16.xml");
        CHECK(sets[4].player == 3 + 3 && sets[4].plots_file == "Later.xml");
        CHECK(sets[5].player == 3 + 5 && sets[5].plots_file == "M5.xml");
    }
}

// q6 t06 (GAME_SCORING.md §1.2 / §1.3): the GameScoring script is re-created at every new session, after the GlobalValue
// clear and before the plots load, so a value its Base_Definitions sets (Thrawn's Revenge: MOD_ID) reaches the plot
// scripts' Base_Definitions.
static void test_scoring_script(const World& w) {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_scoring";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/Scripts/Miscellaneous/GameScoring.lua",
               "ScriptPoolCount = 0\n"
               "function Base_Definitions()\n"
               "  GlobalValue.Set(\"Scoring_Runs\", (GlobalValue.Get(\"Scoring_Runs\") or 0) + 1)\n"
               "  GlobalValue.Set(\"MOD_ID\", \"TEST_MOD\")\n"
               "end\n"
               "function main() end\n");
    write_file(root / "Data/XML/Story_Plots_Scoring.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Scoring</Lua_Script><Active_Plot>Story_Scoring_Plot.xml</Active_Plot>"
               "<Suspended_Plot /></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Scoring_Plot.xml",
               "<Story><Event Name=\"Begin\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event></Story>\n");
    write_file(root / "Data/Scripts/Story/Story_Scoring.lua",
               "function Base_Definitions() GlobalValue.Set(\"Plot_Saw\", GlobalValue.Get(\"MOD_ID\")) end\n"
               "function main() end\n");
    DirectoryProvider files(path_to_utf8(root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    const GalacticRules r = rules();  // GalacticSim keeps a reference
    const GalacticSetup su = setup();
    SimHost host(w.types, js, SimConfig{});
    host.set_scoring_script(&files, "GameScoring");
    for (int session = 0; session < 2; ++session) {
        GalacticSim* gs = host.start_galactic(r, su);
        CHECK(gs != nullptr && host.scoring_vm() != nullptr);
        if (!gs) return;
        for (const std::string& m : host.scoring_messages()) std::printf("  scoring: %s\n", m.c_str());
        CHECK(host.scoring_messages().empty());
        // A fresh state each session over the cleared table: the run counter starts again at 1.
        CHECK(host.global_values()->get("Scoring_Runs").number == 1.0);
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pEmpire, "Story_Plots_Scoring.xml"}};
        CHECK(host.start_story(st));
        CHECK(host.global_values()->get("Plot_Saw").text == "TEST_MOD");
        host.run_fast(3);
        CHECK(host.end_game_mode());  // the stack empties: the next start_galactic is a new session
        CHECK(host.scoring_vm() == nullptr);
    }
    // No name: no script, nothing written.
    SimHost plain(w.types, js, SimConfig{});
    CHECK(plain.start_galactic(r, su) != nullptr);
    CHECK(plain.scoring_vm() == nullptr && plain.global_values()->get("MOD_ID").kind == lua::AiGlobalValue::Nil);
}

// q6 t09: the frames the Service of GAME_SCORING.md §2 pumps at for `rate`: now = (float)frame / (float)30 widened,
// due when there is no LastService or now - LastService > rate (strict, double).
static std::string expected_pumps(double rate, uint64_t ticks) {
    std::string out;
    bool has_last = false;
    double last = 0.0;
    for (uint64_t f = 1; f <= ticks; ++f) {
        const double now = static_cast<double>(static_cast<float>(f) / 30.0f);
        if (has_last && !(now - last > rate)) continue;
        out += "P" + std::to_string(f) + ";";
        last = now;
        has_last = true;
    }
    return out;
}

// Only the "P<frame>;" entries of a scoring log.
static std::string pump_entries(const std::string& log) {
    std::string out;
    size_t i = 0;
    while (i < log.size()) {
        const size_t e = log.find(';', i);
        const std::string tok = log.substr(i, e == std::string::npos ? std::string::npos : e - i);
        if (!tok.empty() && tok[0] == 'P' && tok.size() > 1 && tok[1] >= '0' && tok[1] <= '9') out += tok + ";";
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

// The shared part of the q6 t09 test scripts: Log() appends to GlobalValue Scoring_Log; main logs every pump.
static const char* kScoringCommon =
    "ScriptPoolCount = 0\n"
    "function Log(s) ScoringLog = (ScoringLog or \"\") .. s .. \";\" GlobalValue.Set(\"Scoring_Log\", ScoringLog) end\n"
    "function Game_Mode_Starting_Event(mode, map) Log(\"S:\" .. mode .. \":\" .. tostring(map)) end\n"
    "function Game_Mode_Ending_Event(mode) Log(\"E:\" .. mode) end\n"
    "function Reset_Tactical_Stats() Log(\"R\") end\n";

// q6 t09 (GAME_SCORING.md §2 / §3): the Service pump (ServiceRate / LastService), the release on ScriptExit and on a Lua
// error (thread or event call), the mode starting / ending events, GameRandom.Free_Random off the synced stream.
static void test_scoring_service(const World& w) {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_scoring_service";
    std::error_code ec;
    remove_all_long(root, ec);
    auto script = [&root](const char* name, const std::string& body) {
        write_file(root / "Data/Scripts/Miscellaneous" / (std::string(name) + ".lua"), std::string(kScoringCommon) + body);
    };
    // Pump timing at ServiceRate 1 (30 frames, float rounding) and the mode events around a tactical push.
    script("Scoring_Rate",
           "function Base_Definitions() ServiceRate = 1 end\n"
           "function main() while true do Log(\"P\" .. GetCurrentTime.Frame()) coroutine.yield(true) end end\n");
    // ServiceRate 0: every frame (now grows by 1/30 each frame).
    script("Scoring_Every",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function main() while true do Log(\"P\" .. GetCurrentTime.Frame()) coroutine.yield(true) end end\n");
    script("Scoring_Exit",
           "function Base_Definitions() ServiceRate = 0 n = 0 end\n"
           "function main() while true do n = n + 1 Log(\"P\" .. n) if n == 3 then _ScriptExit() end coroutine.yield(true) end end\n");
    script("Scoring_Thread_Error",
           "function Base_Definitions() ServiceRate = 0 n = 0 end\n"
           "function main() while true do n = n + 1 Log(\"P\" .. n) if n == 2 then error(\"boom\") end coroutine.yield(true) end end\n");
    script("Scoring_Event_Error",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function Game_Mode_Starting_Event(mode, map) error(\"bad start\") end\n"
           "function main() while true do Log(\"P\") coroutine.yield(true) end end\n");
    script("Scoring_No_Rate",
           "function Base_Definitions() ServiceRate = \"10\" end\n"  // not a number: absent
           "function main() while true do Log(\"P\") coroutine.yield(true) end end\n");
    auto free_script = [&script](const char* name, bool draw) {
        script(name, std::string("function Base_Definitions() ServiceRate = 0 sum = 0 end\n"
                                 "function main() while true do ") +
                         (draw ? "sum = sum + GameRandom.Free_Random(1, 100) " : "") +
                         "coroutine.yield(true) end end\n");
    };
    free_script("Scoring_Free", true);
    free_script("Scoring_Plain", false);
    // q6 t03: the players without a story session (Thrawn's Revenge's plugins: Find_Player, PlayerWrapper::Get_Name).
    script("Scoring_Players",
           "function Base_Definitions() ServiceRate = 0 n = 0 end\n"
           "function Game_Mode_Starting_Event(mode, map) local p = Find_Player(\"Empire\") Log(\"N:\" .. p.Get_Name()) end\n"
           "function main() while true do n = n + 1 if n <= 2 then Log(\"F:\" .. Find_Player(\"Underworld\").Get_Faction_Name()) end "
           "coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    const GalacticRules r = rules();
    const GalacticSetup su = setup();
    struct Session {
        std::unique_ptr<SimHost> host;
        std::string log() const { return host->global_values()->get("Scoring_Log").text; }
    };
    auto start = [&](const char* name) {
        Session s;
        s.host = std::make_unique<SimHost>(w.types, js, SimConfig{});
        s.host->set_scoring_script(&files, name);
        s.host->set_scoring_campaign("Scoring_Campaign");
        CHECK(s.host->start_galactic(r, su) != nullptr);
        CHECK(s.host->scoring_vm() != nullptr);
        return s;
    };
    auto dump = [](const Session& s) {
        for (const std::string& m : s.host->scoring_messages()) std::printf("  scoring: %s\n", m.c_str());
    };
    {  // ServiceRate 1 over 200 frames; a land mode pushed at frame 100 and popped at frame 150; Reset_Tactical_Stats.
        Session s = start("Scoring_Rate");
        s.host->run_fast(99);
        s.host->start_game(ModeKind::Land);
        s.host->scoring_mode_started(ModeKind::Land, "Scoring_Map");
        s.host->run_fast(50);
        s.host->scoring_reset_tactical_stats();
        CHECK(s.host->end_game_mode());
        s.host->run_fast(50);
        const std::string l = s.log();
        const std::string pumps = pump_entries(l);
        CHECK(pumps == expected_pumps(1.0, 199));
        CHECK(s.host->scoring_services() == static_cast<uint64_t>(std::count(pumps.begin(), pumps.end(), ';')));
        // The galactic start is delivered before the first pump, the land start at the next frame's Service.
        CHECK(l.rfind("S:Galactic:Scoring_Campaign;P1;", 0) == 0);
        const size_t land = l.find("S:Land:Scoring_Map;");
        CHECK(land != std::string::npos && l.find("P", land) != std::string::npos);
        CHECK(l.find("R;E:Land;") != std::string::npos);
        CHECK(s.host->end_game_mode());  // the last mode: Game_Mode_Ending_Event, then the script goes with the stack
        CHECK(s.host->scoring_vm() == nullptr && s.log().size() > 12 && s.log().substr(s.log().size() - 11) == "E:Galactic;");
        CHECK(s.host->scoring_releases() == 0);
        dump(s);
    }
    {  // ServiceRate 0: one pump per frame.
        Session s = start("Scoring_Every");
        s.host->run_fast(10);
        CHECK(pump_entries(s.log()) == expected_pumps(0.0, 10) && s.host->scoring_services() == 10);
    }
    {  // ScriptExit in the third pump: released right after it, nothing later.
        Session s = start("Scoring_Exit");
        s.host->run_fast(10);
        CHECK(s.log() == "S:Galactic:Scoring_Campaign;P1;P2;P3;");
        CHECK(s.host->scoring_vm() == nullptr && s.host->scoring_releases() == 1 && s.host->scoring_services() == 3);
        CHECK(!s.host->scoring_messages().empty() && s.host->scoring_messages().back().find("released (ScriptExit)") != std::string::npos);
    }
    {  // A Lua error in the thread: released after that pump.
        Session s = start("Scoring_Thread_Error");
        s.host->run_fast(10);
        CHECK(s.log() == "S:Galactic:Scoring_Campaign;P1;P2;");
        CHECK(s.host->scoring_vm() == nullptr && s.host->scoring_releases() == 1);
        CHECK(!s.host->scoring_messages().empty() && s.host->scoring_messages().back().find("released (Lua error)") != std::string::npos);
    }
    {  // A Lua error in an event call: the next due Service releases the script without pumping.
        Session s = start("Scoring_Event_Error");
        s.host->run_fast(5);
        CHECK(s.log().empty() && s.host->scoring_services() == 0);
        CHECK(s.host->scoring_vm() == nullptr && s.host->scoring_releases() == 1 && s.host->scoring_calls() == 1);
        bool named = false;
        for (const std::string& m : s.host->scoring_messages()) named = named || m.find("Game_Mode_Starting_Event: ") != std::string::npos;
        CHECK(named);
    }
    {  // ServiceRate not a number: no Service at all, the events still run, the script stays.
        Session s = start("Scoring_No_Rate");
        s.host->run_fast(40);
        CHECK(s.log() == "S:Galactic:Scoring_Campaign;" && s.host->scoring_services() == 0 && s.host->scoring_vm() != nullptr);
    }
#if defined(ALAMO_LUA_PLAYERS)
    {  // q6 t03: no story session - the scoring VM has a player world of its own over the campaign's players (before,
       // Find_Player gave nil there and the first event call released the script).
        Session s = start("Scoring_Players");
        s.host->run_fast(10);
        dump(s);
        CHECK(s.log() == "N:Empire;F:UNDERWORLD;F:UNDERWORLD;");
        CHECK(s.host->scoring_vm() != nullptr && s.host->scoring_releases() == 0);
        CHECK(s.host->end_game_mode() && s.host->scoring_vm() == nullptr);
    }
#endif
    {  // GameRandom.Free_Random (ranged) draws from the free stream: the synced RNG and the frame CRCs are unchanged.
        Session a = start("Scoring_Free");
        Session b = start("Scoring_Plain");
        a.host->run_fast(60);
        b.host->run_fast(60);
        CHECK(a.host->scoring_releases() == 0 && b.host->scoring_releases() == 0);
        dump(a);
        CHECK(a.host->random().synced().state() == b.host->random().synced().state());
        CHECK(!first_divergence(a.host->crc_log(), b.host->crc_log()).diverged);
    }
}

// q6 t09: GameScoring in the S4 synthetic story campaign (production, conquest, a space mode pushed and popped): the
// galactic event calls arrive in stream order after T9, and the run is identical for every worker count (rule 3).
static const char* kScoringStory =
    "function Base_Definitions() ServiceRate = 0.1 end\n"
    "function main() while true do Log(\"P\" .. GetCurrentTime.Frame()) GameRandom(1, 6) coroutine.yield(true) end end\n"
    "function Galactic_Production_Begin_Event(planet, t) Log(\"PB:\" .. tostring(planet ~= nil) .. tostring(t ~= nil)) end\n"
    "function Galactic_Production_Canceled_Event(planet, t) Log(\"PC\") end\n"
    "function Galactic_Production_End_Event(planet, o) Log(\"PE:\" .. tostring(planet ~= nil) .. tostring(o ~= nil)) end\n"
    "function Galactic_Planet_Faction_Change(planet, n, o) Log(\"FC:\" .. tostring(planet ~= nil and n ~= nil and o ~= nil)) end\n"
    "function Galactic_Unit_Destroyed_Event(object, killer) Log(\"UD\") end\n";

static void test_scoring_story(const World& w) {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_scoring_story";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Host_Test.xml", kPlots);
    write_file(root / "Data/XML/Story_Host_Test.xml", kPlot);
    write_file(root / "Data/Scripts/Story/Story_Host_Test.lua", kScript);
    write_file(root / "Data/Scripts/Miscellaneous/GameScoring.lua", std::string(kScoringCommon) + kScoringStory);
    DirectoryProvider files(path_to_utf8(root));
    const uint64_t ticks = 400;
    const Run a = run(w, files, 0, ticks, "GameScoring");
    for (const std::string& m : a.scoring_messages) std::printf("  scoring: %s\n", m.c_str());
    std::printf("scoring story: %" PRIu64 " services, %" PRIu64 " calls, log %zu bytes, final CRC %016" PRIx64 "\n",
                a.scoring_services, a.scoring_calls, a.scoring_log.size(), a.crc.records.empty() ? 0 : a.crc.records.back().combined);
    const std::string& l = a.scoring_log;
    std::string events;  // the log without the pumps
    for (size_t i = 0; i < l.size();) {
        const size_t e = l.find(';', i);
        const std::string tok = l.substr(i, e - i);
        if (!(tok.size() > 1 && tok[0] == 'P' && tok[1] >= '0' && tok[1] <= '9')) events += tok + ";";
        if (e == std::string::npos) break;
        i = e + 1;
    }
    std::printf("  scoring story events: %s\n", events.c_str());
    CHECK(a.scoring_releases == 0);
    CHECK(l.rfind("S:Galactic:StoryTest;P1;", 0) == 0);
    CHECK(pump_entries(l) == expected_pumps(0.1, ticks));
    auto count = [&l](const std::string& tok) {
        size_t n = 0;
        for (size_t i = l.find(tok); i != std::string::npos; i = l.find(tok, i + 1)) ++n;
        return n;
    };
    // The two fighters (the type wrapper is nil here: the synthetic galactic rules' type indices are past the test
    // World's object types; with game data they are the same table).
    CHECK(count("PB:true") == 2);
    CHECK(count("PE:truetrue;") == 2);
    CHECK(count("FC:true;") >= 1);      // Underworld takes C
    CHECK(count("S:Space:Test_Space_Map;") == 1 && count("E:Space;") == 1);
    CHECK(l.find("S:Space:Test_Space_Map;") < l.find("E:Space;"));
    // A scoring run differs from the plain one (its GlobalValue and RNG draws are sim state) but not across workers.
    const Run plain = run(w, files, 0, ticks);
    CHECK(first_divergence(a.crc, plain.crc).diverged);
    for (int32_t workers : jobs::determinism_worker_counts()) {
        if (workers == 0) continue;
        const Run b = run(w, files, workers, ticks, "GameScoring");
        const auto d = first_divergence(a.crc, b.crc);
        std::printf("  scoring story workers %2d: %s, scoring log %s\n", workers, d.message.c_str(),
                    b.scoring_log == a.scoring_log ? "identical" : "DIFFERS");
        CHECK(!d.diverged && b.scoring_log == a.scoring_log && b.log == a.log);
    }
}

// q6 (Thrawn's Revenge story chain): (1) a tick-2 event (ELAPSED 0, A2i t01) reaches a PGStoryMode-style script whose
// state machine starts at load - Story_Event_Trigger only switches state when CurrentState == NextState, which holds
// only after main's first resume; the load-time resume (StorySession::start_threads_at_load) puts it there before the first T9's STORY_ELAPSED
// dispatch (T9 step 2) - (2) a prereq-less Perpetual STORY_ELAPSED fires first where the T9 clock has accumulated
// Param1 seconds since the first T9; the Perpetual reset clears the accumulator and the initialised flag, so the next
// T9 re-records the start and the next fire comes Param1 seconds after that (STORY.md §4.7 [H]: period Param1 plus one
// T9), (3) SWITCH_CONTROL rewards reach the host hook (SimHost::set_switch_control_hook), from an XML reward (faction
// name) and from a Lua Set_Reward_Parameter(0, faction name) like StoryUtil.ChangeAIPlayer (a player object is not a
// converter value: LUA_API.md "StoryEventWrapper dialog and parameter setters"). Same results for every worker count.
static const char* kQ6Plot =
    "<Story>\n"
    " <Event Name=\"Begin\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n"
    " <Event Name=\"Cycle\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>2</Event_Param1><Perpetual>True</Perpetual></Event>\n"
    " <Event Name=\"Switch_Xml\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>SWITCH_XML</Event_Param2>"
    "<Reward_Type>SWITCH_CONTROL</Reward_Type><Reward_Param1>Underworld</Reward_Param1><Reward_Param2>TestAI</Reward_Param2></Event>\n"
    " <Event Name=\"Switch_Lua\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>SWITCH_LUA</Event_Param2>"
    "<Reward_Type>SWITCH_CONTROL</Reward_Type></Event>\n"
    "</Story>\n";
// PGStateMachine.lua's rule in miniature: main = Process_States moves CurrentState to NextState; a story event only
// requests a state change while the machine is settled (CurrentState == NextState).
static const char* kQ6Script =
    "function Base_Definitions() ServiceRate = 0.1 NextState = \"Init\" end\n"
    "function Story_Event_Trigger(name)\n"
    "  if CurrentState ~= NextState then return end\n"
    "  if name == \"Begin\" and CurrentState == \"Init\" then NextState = \"Started\" end\n"
    "end\n"
    "function main()\n"
    "  while true do\n"
    "    if CurrentState ~= NextState then\n"
    "      CurrentState = NextState\n"
    "      if CurrentState == \"Started\" then\n"
    "        Story_Event(\"SWITCH_XML\")\n"
    "        local ev = Get_Story_Plot(\"Story_Q6.xml\").Get_Event(\"Switch_Lua\")\n"
    "        ev.Set_Reward_Parameter(0, \"Empire\")\n"
    "        ev.Set_Reward_Parameter(1, \"OtherAI\")\n"
    "        Story_Event(\"SWITCH_LUA\")\n"
    "      end\n"
    "    end\n"
    "    coroutine.yield(true)\n"
    "  end\n"
    "end\n";

static void test_q6_story_chain(const World& w) {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_q6_story_chain";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Q6.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Q6</Lua_Script><Active_Plot>Story_Q6.xml</Active_Plot>"
               "<Suspended_Plot /></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Q6.xml", kQ6Plot);
    write_file(root / "Data/Scripts/Story/Story_Q6.lua", kQ6Script);
    DirectoryProvider files(path_to_utf8(root));
    constexpr uint64_t kTicks = 150;
    std::string first_log, first_switch;
    for (int32_t workers : jobs::determinism_worker_counts()) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 7;
        cfg.story_crc = true;
        SimHost host(w.types, js, cfg);
        std::vector<StorySwitchControl> switches;
        host.set_switch_control_hook([&switches](const StorySwitchControl& sw) { switches.push_back(sw); });
        const GalacticRules r = rules();
        GalacticSim* gs = host.start_galactic(r, setup());
        CHECK(gs != nullptr);
        if (!gs) return;
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pEmpire, "Story_Plots_Q6.xml"}};
        std::vector<std::string> diag;
        CHECK(host.start_story(st, &diag) && host.story());
        for (const auto& d : diag) std::printf("  q6 story diag: %s\n", d.c_str());
        if (!host.story()) return;
        std::vector<std::pair<uint64_t, double>> clock;  // global frame after the tick, the T9 clock of that tick
        for (uint64_t t = 1; t <= kTicks; ++t) {
            const uint64_t seen = host.galactic_mode()->frame();
            host.tick_once();
            clock.emplace_back(host.frame(), StorySession::t9_seconds(seen));
        }
        for (const std::string& m : host.story()->lua_messages()) std::printf("  q6 story lua: %s\n", m.c_str());
        CHECK(host.story()->lua_messages().empty());
        std::map<std::string, std::vector<uint64_t>> fired;
        std::string log;
        for (const StoryLogEntry& e : host.story()->log()) {
            fired[e.event].push_back(e.frame);
            log += e.text() + "\n";
        }
        // (1) Begin at the second T9 (a prereq-less ELAPSED 0 records LastTime at its first T9 and fires at the next:
        // A2i t01, 0x454980); its Story_Event_Trigger switched the settled machine, so the next Lua service (ServiceRate
        // 0.1 s: T9 0 s at the first tick, then the tick whose T9 reaches 0.1 s, 3 frames later) entered "Started" and
        // raised both notifications in the same frame.
        const uint64_t f0 = clock.size() > 1 ? clock[1].first : 0;
        const uint64_t f1 = clock.size() > 3 ? clock[3].first : 0;
        CHECK(fired["Begin"] == std::vector<uint64_t>{f0});
        CHECK(fired["Switch_Xml"] == std::vector<uint64_t>{f1});
        CHECK(fired["Switch_Lua"] == std::vector<uint64_t>{f1});
        // (3) both rewards reached the hook, resolved to players.
        CHECK(switches.size() == 2);
        std::string sw_text;
        for (const StorySwitchControl& sw : switches)
            sw_text += std::to_string(sw.frame) + " " + std::to_string(sw.plot_set) + " " + std::to_string(sw.player) + " " +
                       sw.player_text + " " + sw.ai_type + "\n";
        CHECK(switches.size() == 2 && switches[0].player == pUnder && switches[0].ai_type == "TestAI" &&
              switches[0].player_text == "Underworld" && switches[0].frame == f1);
        CHECK(switches.size() == 2 && switches[1].player == pEmpire && switches[1].ai_type == "OtherAI" &&
              switches[1].player_text == "Empire");
        // (2) Cycle: the first T9 records LastTime, later T9s add now - LastTime; a fire at accumulated >= 2 s clears
        // the accumulator and the flag, the T9 after it records LastTime again.
        std::vector<uint64_t> expect;
        {
            double acc = 0.0, last = 0.0;
            bool init = false;
            for (const auto& [frame, now] : clock) {
                if (!init) {
                    init = true;
                    last = now;
                    continue;
                }
                acc += now - last;
                last = now;
                if (acc >= 2.0) {
                    expect.push_back(frame);
                    acc = 0.0;
                    init = false;
                }
            }
        }
        const uint64_t first_fire = expect.empty() ? 0 : expect.front();
        std::printf("  q6 story chain, workers %2d: Begin @%" PRIu64 ", Cycle first @%" PRIu64 " (x%zu), %zu SWITCH_CONTROL\n",
                    workers, f0, fired["Cycle"].empty() ? 0 : fired["Cycle"].front(), fired["Cycle"].size(), switches.size());
        CHECK(first_fire >= 60 && first_fire <= 62);  // 2 s = 60 galactic frames after the first T9 (+-1: float clock)
        CHECK(fired["Cycle"] == expect && expect.size() >= 2 && expect[1] - expect[0] >= 60 && expect[1] - expect[0] <= 63);
        if (first_log.empty()) {
            first_log = log;
            first_switch = sw_text;
        } else {
            CHECK(log == first_log && sw_text == first_switch);
        }
    }
    remove_all_long(root, ec);
}

// bkc1 t01 (docs/re/BKC1_RE_ANSWERS.md Q1 [H]): Story_Handle_Esc. The key goes to the story only while the cinematic
// camera is on (Start_Cinematic_Camera, the controller's state 4); handle_esc() offers the call to every ACTIVE plot of
// the current mode in plot-table order, calling the plot script's global only when it is a function (no arguments); a
// suspended plot (D) is skipped, a plot without a script (B) or without the function is no error. A thread the handler
// creates first runs at the next T9.
static void test_story_handle_esc(const World& w) {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_handle_esc";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Esc.xml",
               "<Story_Mode_Plots><Lua_Script>Story_EscA</Lua_Script><Active_Plot>Story_EscA.xml</Active_Plot>"
               "<Active_Plot>Story_EscB.xml</Active_Plot>"
               "<Lua_Script>Story_EscC</Lua_Script><Active_Plot>Story_EscC.xml</Active_Plot>"
               "<Lua_Script>Story_EscD</Lua_Script><Suspended_Plot>Story_EscD.xml</Suspended_Plot>"
               "<Lua_Script>Story_EscE</Lua_Script><Active_Plot>Story_EscE.xml</Active_Plot></Story_Mode_Plots>\n");
    const char* kEmpty = "<Story><Event Name=\"Idle\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>NEVER</Event_Param2></Event></Story>\n";
    write_file(root / "Data/XML/Story_EscA.xml", kEmpty);
    write_file(root / "Data/XML/Story_EscC.xml", kEmpty);
    write_file(root / "Data/XML/Story_EscD.xml", kEmpty);
    write_file(root / "Data/XML/Story_EscE.xml", kEmpty);
    write_file(root / "Data/XML/Story_EscB.xml",
               "<Story>\n"
               " <Event Name=\"Seen_A\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>ESC_A</Event_Param2><Perpetual>Yes</Perpetual></Event>\n"
               " <Event Name=\"Seen_C\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>ESC_C</Event_Param2><Perpetual>Yes</Perpetual></Event>\n"
               " <Event Name=\"Seen_D\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>ESC_D</Event_Param2></Event>\n"
               " <Event Name=\"After_A\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>AFTER_A</Event_Param2></Event>\n"
               "</Story>\n");
    write_file(root / "Data/Scripts/Story/Story_EscA.lua",
               "function Base_Definitions() ServiceRate = 0.01 end\n"
               "function main() Start_Cinematic_Camera() end\n"
               "function After() Story_Event(\"AFTER_A\") end\n"
               "function Story_Handle_Esc() Story_Event(\"ESC_A\") Create_Thread(\"After\") End_Cinematic_Camera() end\n");
    write_file(root / "Data/Scripts/Story/Story_EscC.lua",
               "function Base_Definitions() ServiceRate = 0.01 end\nfunction main() end\n"
               "function Story_Handle_Esc(a) if a == nil then Story_Event(\"ESC_C\") end end\n");
    write_file(root / "Data/Scripts/Story/Story_EscD.lua",
               "function Base_Definitions() end\nfunction main() end\nfunction Story_Handle_Esc() Story_Event(\"ESC_D\") end\n");
    write_file(root / "Data/Scripts/Story/Story_EscE.lua",
               "function Base_Definitions() end\nfunction main() end\nStory_Handle_Esc = 5\n");  // not a function: skipped
    DirectoryProvider files(path_to_utf8(root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 3;
    SimHost host(w.types, js, cfg);
    const GalacticRules r = rules();
    GalacticSim* gs = host.start_galactic(r, setup());
    CHECK(gs != nullptr);
    if (!gs) return;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{pEmpire, "Story_Plots_Esc.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag) && host.story());
    if (!host.story()) return;
    StorySession& story = *host.story();
    for (int t = 0; t < 5; ++t) host.tick_once();
    CHECK(story.esc_goes_to_story());  // A's main (first resumed at story start) ran Start_Cinematic_Camera
    auto fired = [&story]() {
        std::string out;
        for (const StoryLogEntry& e : story.log())
            if (e.event.rfind("Seen_", 0) == 0 || e.event == "After_A") out += e.event + ",";
        return out;
    };
    CHECK(fired().empty());
    CHECK(story.handle_esc() == 2);  // A then C, in plot-table order; D suspended, B no script, E not a function
    CHECK(fired() == "Seen_A,Seen_C,");
    CHECK(!story.esc_goes_to_story());  // A's handler ended the cinematic camera: Esc opens the host's pause menu again
    const uint64_t esc_frame = host.frame();
    host.tick_once();
    CHECK(fired() == "Seen_A,Seen_C,After_A,");  // the thread created by the handler ran at the next tick's T9
    uint64_t after = 0;
    for (const StoryLogEntry& e : story.log())
        if (e.event == "After_A") after = e.frame;
    CHECK(after == esc_frame + 1);
    for (const std::string& m : story.lua_messages()) std::printf("  story esc lua: %s\n", m.c_str());
    CHECK(story.lua_messages().empty());
    std::printf("  story handle esc: 2 plots called, After_A at frame %" PRIu64 " (Esc after frame %" PRIu64 ")\n", after,
                esc_frame);
    remove_all_long(root, ec);
}
}  // namespace story_test

// q6: game-object Lua scripts (object_scripts.hpp, docs/re/OBJECT_SCRIPTS.md) on synthetic land types and scripts.
namespace object_script_test {

struct OsWorld {
    fs::path root;
    std::unique_ptr<DirectoryProvider> files;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
};

static void script(const fs::path& root, const char* name, const std::string& body) {
    write_file(root / "Data/Scripts/GameObject" / (std::string(name) + ".lua"), body);
}

static std::unique_ptr<OsWorld> make_os_world() {
    auto w = std::make_unique<OsWorld>();
    w->root = fs::temp_directory_path() / "alamo_sim_object_scripts";
    std::error_code ec;
    remove_all_long(w->root, ec);
    write_file(w->root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>Os.xml</File></GameObjectFiles>\n");
    std::string xml = "<Os>\n";
    for (const char* n : {"Os_Attach", "Os_Gate", "Os_NoRate", "Os_Exit", "Os_ExitDefs", "Os_BindErr", "Os_ThreadErr", "Os_Death",
                          "Os_Prox", "Os_Spawner", "Os_Missing", "Os_Pool", "Os_Pool3", "Os_Pool0", "Os_Save"})
        xml += std::string(" <GroundInfantry Name=\"") + n + "\"><Tactical_Health>10</Tactical_Health><Lua_Script>" + n +
               (std::string(n) == "Os_Gate" ? ".lua" : "") + "</Lua_Script></GroundInfantry>\n";
    xml += " <GroundInfantry Name=\"Os_Attach_Variant\"><Variant_Of_Existing_Type>Os_Attach</Variant_Of_Existing_Type></GroundInfantry>\n"
           " <GroundInfantry Name=\"Os_Plain\"><Tactical_Health>10</Tactical_Health></GroundInfantry>\n"
           " <GroundInfantry Name=\"Os_Unit\"><Tactical_Health>10</Tactical_Health></GroundInfantry>\n"
           // q6 t19 (§1.5): a type instance from the Os_Pool4 pool (ScriptPoolCount 4).
           " <GroundInfantry Name=\"Os_TypeScript\"><Tactical_Health>10</Tactical_Health><Lua_Script>Os_Pool4</Lua_Script>"
           "<Execute_Script_On_Type>Yes</Execute_Script_On_Type></GroundInfantry>\n"
           "</Os>\n";
    write_file(w->root / "Data/XML/Os.xml", xml);
    const fs::path& r = w->root;
    // Attach order: Object is nil in the top level and in Base_Definitions, set before the first pump; Get_Name upper-case.
    script(r, "Os_Attach",
           "TopSaw = tostring(Object)\n"
           "function Base_Definitions() ServiceRate = 0 GlobalValue.Set('BD', TopSaw .. '/' .. tostring(Object)) end\n"
           "function main() GlobalValue.Set('Main', Object.Get_Type().Get_Name() .. '/' .. Object.Get_Object_ID()) "
           "while true do coroutine.yield(true) end end\n");
    script(r, "Os_Gate",
           "function Base_Definitions() ServiceRate = 1 end\n"
           "function main() while true do GlobalValue.Set('Gate', (GlobalValue.Get('Gate') or '') .. GetCurrentTime.Frame() .. ';') "
           "coroutine.yield(true) end end\n");
    script(r, "Os_NoRate", "function main() GlobalValue.Set('NoRate', 1) end\n");
    script(r, "Os_Exit",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function main() GlobalValue.Set('Exit', (GlobalValue.Get('Exit') or 0) + 1) _ScriptExit() coroutine.yield(false) end\n");
    script(r, "Os_ExitDefs",
           "function Base_Definitions() ServiceRate = 0 _ScriptExit() end\n"
           "function main() GlobalValue.Set('ExitDefs', 1) end\n");
    script(r, "Os_BindErr",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function main() Object.Event_Object_In_Range(5, 1) GlobalValue.Set('BindErr', (GlobalValue.Get('BindErr') or 0) + 1) "
           "coroutine.yield(true) GlobalValue.Set('BindErrLater', 1) end\n");
    script(r, "Os_ThreadErr",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function Second() while true do GlobalValue.Set('Second', (GlobalValue.Get('Second') or 0) + 1) coroutine.yield(true) end end\n"
           "function main() Create_Thread('Second') coroutine.yield(true) error('boom') end\n");
    script(r, "Os_Death",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function main() while true do GlobalValue.Set('Death', (GlobalValue.Get('Death') or 0) + 1) coroutine.yield(true) end end\n");
    script(r, "Os_Prox",
           "function Base_Definitions() ServiceRate = 0 end\n"
           "function Hit(o, t) GlobalValue.Set('Hits', (GlobalValue.Get('Hits') or '') .. t.Get_Object_ID() .. ',') end\n"
           "function main() Object.Event_Object_In_Range(Hit, 50) while true do Object.Service_Wrapper() "
           "GlobalValue.Set('Hits', (GlobalValue.Get('Hits') or '') .. '|') coroutine.yield(true) end end\n");
    // A miniature TR GenericCompanySpawner (OBJECT_SCRIPTS.md §6.1): exit outside land, spawn the company at the dummy,
    // hide it, kill the dummy once 40% or less of the company lives. GameRandom draws on the synced stream.
    script(r, "Os_Spawner",
           "function Base_Definitions() ServiceRate = 0.1 Units = {} end\n"
           "function main()\n"
           "  if Get_Game_Mode() ~= 'Land' then _ScriptExit() coroutine.yield(false) end\n"
           "  local t = Find_Object_Type('Os_Unit')\n"
           "  local x, y, z = Object.Get_Position().Get_XYZ()\n"
           "  for i = 1, 3 do\n"
           "    local u = Spawn_Unit(t, Create_Position(x + 5 * i, y + GameRandom(1, 4), z), Object.Get_Owner())[1]\n"
           "    table.insert(Units, u)\n"
           "  end\n"
           "  Object.Hide(true)\n"
           "  while true do\n"
           "    local alive = 0\n"
           "    for _, u in ipairs(Units) do if u.Is_Valid() then alive = alive + 1 end end\n"
           "    if alive / table.getn(Units) <= 0.4 then GlobalValue.Set('SpawnerDone', alive) Object.Take_Damage(999999) end\n"
           "    coroutine.yield(true)\n"
           "  end\n"
           "end\n");
    // q6 t19 pool (§1.4, §2.3): no ScriptPoolCount (pool of 1, grows), 3 (preloaded), 0 (no pool), 4 (the type instance's).
    // TopRuns counts this VM's top-level runs; Kept (a number) survives Flush_G; Flush_G / Base_Definitions count in
    // GlobalValue (absent at the top level: the engine globals are attached after it, §1.3).
    const std::string pool_body =
        "TopRuns = (TopRuns or 0) + 1\n"
        "Kept = 0\n"
        "function Flush_G() GlobalValue.Set(Tag .. 'Flush', (GlobalValue.Get(Tag .. 'Flush') or 0) + 1) Tab = nil end\n"
        "function Base_Definitions() ServiceRate = 0 LastService = nil Tab = {} "
        "GlobalValue.Set(Tag .. 'BD', (GlobalValue.Get(Tag .. 'BD') or 0) + 1) GlobalValue.Set(Tag .. 'Obj', tostring(Object)) end\n"
        "function main() Kept = Kept + 1 GlobalValue.Set(Tag .. 'Main', (GlobalValue.Get(Tag .. 'Main') or '') .. GetThreadID() .. '/' "
        ".. Kept .. '/' .. TopRuns .. '/' .. Object.Get_Object_ID() .. ';') while true do coroutine.yield(true) end end\n";
    script(r, "Os_Pool", "Tag = 'P'\n" + pool_body);
    script(r, "Os_Pool3", "Tag = 'P3'\nScriptPoolCount = 3\n" + pool_body);
    script(r, "Os_Pool0", "Tag = 'P0'\nScriptPoolCount = 0\n" + pool_body);
    script(r, "Os_Pool4", "Tag = 'P4'\nScriptPoolCount = 4\n" + pool_body);
    // q6 t19 save / load (§2.6): OnEnter-style work counted once (SaveOnEnter), `main` sleeping in an inline PGBase Sleep,
    // a second thread waiting for a flag set by main, a closure counter (upvalue), a cycle and a shared table, the Object
    // wrapper kept in a table (identity), a first-pump draw (ServiceRate > 0). Every pump writes a GlobalValue (frame CRC).
    script(r, "Os_Save",
           "TopRuns = (TopRuns or 0) + 1\n"
           "function Sleep(t) local s = GetCurrentTime() while GetCurrentTime() - s < t do coroutine.yield(true) end end\n"
           "local function counter() local c = 0 return function() c = c + 1 return c end end\n"
           "function Base_Definitions() ServiceRate = 0.1 Ring = {} Ring.self = Ring Shared = {n = 0} "
           "Graph = {a = Shared, b = Shared, list = {Shared, Ring}} end\n"
           "function Waiter() while true do if Signal then Signal = nil Signals = (Signals or 0) + 1 "
           "GlobalValue.Set('SaveSig' .. Object.Get_Object_ID(), Signals) end coroutine.yield(true) end end\n"
           "function main()\n"
           "  GlobalValue.Set('SaveOnEnter', (GlobalValue.Get('SaveOnEnter') or 0) + 1)\n"
           "  local tick = counter()\n"
           "  Mine = {obj = Object}\n"
           "  Create_Thread('Waiter')\n"
           "  while true do\n"
           "    local n = tick()\n"
           "    Shared.n = Shared.n + 1\n"
           "    if n == 3 or n == 9 or n == 20 then Signal = true end\n"
           "    GlobalValue.Set('Save' .. Object.Get_Object_ID(), n .. '/' .. Graph.a.n .. '/' .. tostring(Graph.a == Graph.b) .. '/' "
           "..  tostring(Graph.list[2].self == Ring) .. '/' .. tostring(Mine.obj == Object) .. '/' .. TopRuns)\n"
           "    Sleep(0.25)\n"
           "  end\n"
           "end\n");
    w->files = std::make_unique<DirectoryProvider>(path_to_utf8(w->root));
    CHECK(w->db.load(*w->files));
    CHECK(w->types.build(w->db));
    return w;
}

static ObjectHandle spawn(SimHost& host, const OsWorld& w, const char* type, int32_t owner, Vec3 pos) {
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(w.db.find(type));
    sp.owner = owner;
    sp.position = pos;
    sp.facing = {1.0f, 0.0f, 0.0f};
    return host.tactical()->spawn(sp);
}

static std::string gv_text(SimHost& host, const char* key) { return host.global_values()->get(key).text; }
static double gv_num(SimHost& host, const char* key) { return host.global_values()->get(key).number; }

static SimConfig os_config(bool on) {
    SimConfig cfg;
    cfg.seed = 4242;
    cfg.object_scripts = on;
    return cfg;
}

static void test_attach_and_gate(const OsWorld& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    {   // Attach order, inherited Lua_Script, Get_Name upper-case; an object without a script attaches nothing.
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        host.start_game(ModeKind::Land);
        const ObjectHandle plain = spawn(host, w, "Os_Plain", 0, {0, 0, 0});
        const ObjectHandle v = spawn(host, w, "Os_Attach_Variant", 0, {0, 0, 0});
        ObjectScriptHost& os = *host.object_scripts();
        CHECK(os.live() == 1 && os.stats().attached == 1 && os.has_script(host.tactical()->primary(), v) &&
              !os.has_script(host.tactical()->primary(), plain));
        CHECK(gv_text(host, "BD") == "nil/nil");  // Object nil in the top level and in Base_Definitions
        CHECK(gv_text(host, "Main").empty());     // main created, not run
        host.run_fast(1);
        CHECK(gv_text(host, "Main") == "OS_ATTACH_VARIANT/" + std::to_string(host.tactical()->primary().id(v)));
        std::string why;  // q6 t19: refused for the bare tactical mode (no rebuilder), not for the scripts
        CHECK(!host.save_allowed(&why) && why.find("game-object") == std::string::npos);
        // A missing file: no script (counted).
        spawn(host, w, "Os_Missing", 0, {0, 0, 0});
        CHECK(os.live() == 1 && os.stats().load_failures == 1 && os.error_total() == 1);
        // Mode end releases the store's scripts.
        CHECK(host.end_game_mode() && os.live() == 0 && os.stats().released_mode == 1);
    }
    {   // Off (default): nothing attaches, no host.
        SimHost host(w.types, js, os_config(false));
        host.start_game(ModeKind::Land);
        spawn(host, w, "Os_Attach", 0, {0, 0, 0});
        CHECK(host.object_scripts() == nullptr && host.global_values()->get("BD").kind == lua::AiGlobalValue::Nil);
    }
    {   // The gate (§3.2): ServiceRate 1, the first pump at mode frame 0 sets LastService = -(one synced draw in [0, 1]),
        // then now = (float)frame x 0.033333335f, due when now - LastService > 1 (strict, double).
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        host.start_game(ModeKind::Land);
        spawn(host, w, "Os_Gate", 0, {0, 0, 0});
        spawn(host, w, "Os_NoRate", 0, {0, 0, 0});  // no ServiceRate: never pumped, no draw
        SimRng copy = host.random().synced();
        const double r = static_cast<double>(copy.range_float(0.0f, first_pump_draw_hi(1.0)));
        host.run_fast(200);
        const std::string got = gv_text(host, "Gate");
        const uint64_t f0 = std::strtoull(got.c_str(), nullptr, 10);
        std::string want;
        size_t pumps = 0;
        double last = -r;
        for (uint64_t f = f0; f < f0 + 400; ++f) {
            const double now = object_script_time(f);
            if (f != f0 && !(now - last > 1.0)) continue;
            want += std::to_string(f) + ";";
            if (f != f0) last = now;
            if (want.size() <= got.size()) ++pumps;
        }
        CHECK(!got.empty() && want.compare(0, got.size(), got) == 0 && pumps >= 6);
        CHECK(host.object_scripts()->stats().pumps == pumps);
        CHECK(host.global_values()->get("NoRate").kind == lua::AiGlobalValue::Nil);
        CHECK(host.object_scripts()->stats().first_draws == 1);
        CHECK(host.random().synced().state() == copy.state());  // exactly the one draw
        std::printf("object scripts gate: first draw %.6f, %zu pumps at mode frames %s\n", r, pumps, got.c_str());
    }
}

static void test_release(const OsWorld& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimHost host(w.types, js, os_config(true));
    host.set_object_script_files(w.files.get());
    host.start_game(ModeKind::Land);
    ObjectScriptHost& os = *host.object_scripts();
    spawn(host, w, "Os_Exit", 0, {0, 0, 0});
    spawn(host, w, "Os_ExitDefs", 0, {0, 0, 0});
    spawn(host, w, "Os_BindErr", 0, {0, 0, 0});
    spawn(host, w, "Os_ThreadErr", 0, {0, 0, 0});
    const ObjectHandle dying = spawn(host, w, "Os_Death", 0, {0, 0, 0});
    CHECK(os.live() == 5);
    host.run_fast(3);
    // ScriptExit in a thread: one pump, then released. ScriptExit in Base_Definitions: released before main runs.
    CHECK(gv_num(host, "Exit") == 1.0 && host.global_values()->get("ExitDefs").kind == lua::AiGlobalValue::Nil);
    // A binding script error (+0x121): the thread runs on to its yield, then the script is released.
    CHECK(gv_num(host, "BindErr") == 1.0 && host.global_values()->get("BindErrLater").kind == lua::AiGlobalValue::Nil);
    CHECK(os.stats().released_exit == 3);
    // A Lua runtime error kills only its thread: the second thread keeps running.
    const double second = gv_num(host, "Second");
    host.run_fast(2);
    CHECK(second >= 1.0 && gv_num(host, "Second") == second + 2.0 && os.stats().thread_errors == 1);
    // Death stops the service at once and releases the script.
    const double deaths = gv_num(host, "Death");
    CHECK(deaths == 5.0);
    host.tactical()->destroy(dying);
    host.run_fast(3);
    CHECK(gv_num(host, "Death") == deaths && os.stats().released_dead == 1 && os.live() == 1);
    bool boom = false;
    for (const auto& [text, n] : os.error_counts()) boom = boom || (text.find("boom") != std::string::npos && n == 1);
    CHECK(boom);
}

static void test_proximity_and_spawner(const OsWorld& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    {   // Proximity (§4.3): synchronous callbacks inside Service_Wrapper, box of half-size 50, ascending id.
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        host.start_game(ModeKind::Land);
        const ObjectHandle far_a = spawn(host, w, "Os_Plain", 1, {40, 40, 0});    // in the box (not in a 50 sphere)
        spawn(host, w, "Os_Plain", 1, {60, 0, 0});                                // outside
        const ObjectHandle prox = spawn(host, w, "Os_Prox", 0, {0, 0, 0});
        const ObjectHandle near_b = spawn(host, w, "Os_Plain", 2, {-10, 0, 0});
        host.run_fast(2);
        const objects::ObjectStore& st = host.tactical()->primary();
        const std::string a = std::to_string(st.id(far_a)), b = std::to_string(st.id(near_b));
        CHECK(gv_text(host, "Hits") == a + "," + b + ",|" + a + "," + b + ",|");
        (void)prox;
    }
    {   // The mini company spawner: units spawned in the first pump, the dummy dies when <= 40% of them live.
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        host.start_game(ModeKind::Land);
        const ObjectHandle dummy = spawn(host, w, "Os_Spawner", 1, {100, 0, 0});
        host.run_fast(1);
        const objects::ObjectStore& st = host.tactical()->primary();
        const std::vector<ObjectHandle> units = st.find_all_of_type(static_cast<objects::TypeIndex>(w.db.find("Os_Unit")));
        CHECK(units.size() == 3);
        for (const ObjectHandle u : units) CHECK(st.owner(u) == 1);
        CHECK(host.object_scripts()->live() == 1);  // the spawned units have no script
        host.run_fast(10);
        CHECK(st.alive(dummy));
        host.tactical()->destroy(units[0]);
        host.tactical()->destroy(units[1]);
        host.run_fast(10);
        CHECK(!st.alive(dummy) && gv_num(host, "SpawnerDone") == 1.0 && host.object_scripts()->live() == 0 &&
              host.object_scripts()->stats().released_dead == 1);
    }
    {   // Galactic store: Get_Game_Mode is "Galactic", the spawner exits on its first pump; q6 t19: a live script no
        // longer blocks the save.
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        GameMode& gal = host.start_game(ModeKind::Galactic);
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(w.db.find("Os_Attach"));
        gal.primary().create(sp);
        std::string why;
        CHECK(host.object_scripts()->live() == 1 && host.save_allowed(&why));
        sp.type = static_cast<objects::TypeIndex>(w.db.find("Os_Spawner"));
        gal.primary().create(sp);
        host.run_fast(1);
        CHECK(host.object_scripts()->live() == 1 && host.object_scripts()->stats().released_exit == 1);
    }
}

// Rule 3: the scripts (attach, first-pump draws, GameRandom in a pump, Spawn_Unit) give the same CRCs at every worker count.
static void test_worker_counts(const OsWorld& w) {
    CrcLog first;
    for (const int32_t workers : {0, 1, 4}) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimHost host(w.types, js, os_config(true));
        host.set_object_script_files(w.files.get());
        host.start_game(ModeKind::Land);
        for (int i = 0; i < 4; ++i) spawn(host, w, "Os_Spawner", i % 2, {static_cast<float>(100 * i), 0, 0});
        spawn(host, w, "Os_Gate", 0, {0, 0, 0});
        spawn(host, w, "Os_Prox", 1, {150, 0, 0});
        // q6 t19: a pooled instance released and handed out again mid-run (each new holder draws on its first pump).
        const ObjectHandle p1 = spawn(host, w, "Os_Pool", 0, {20, 0, 0});
        spawn(host, w, "Os_Pool", 1, {30, 0, 0});
        host.run_fast(30);
        host.tactical()->destroy(p1);
        host.run_fast(10);
        spawn(host, w, "Os_Pool", 0, {40, 0, 0});
        host.run_fast(80);
        if (workers == 0) {
            first = host.crc_log();
            CHECK(host.object_scripts()->stats().first_draws == 9 && host.object_scripts()->stats().reused == 1);
        } else {
            CHECK(!first_divergence(first, host.crc_log()).diverged);
        }
    }
}

// q6 t19: the script pool (§1.4, §2.3) and the type instance (§1.5).
static const ObjectScriptHost::PoolInfo* pool_of(const std::vector<ObjectScriptHost::PoolInfo>& pools, const char* name) {
    for (const ObjectScriptHost::PoolInfo& p : pools)
        if (p.name == name) return &p;
    return nullptr;
}

static void test_pool(const OsWorld& w) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimHost host(w.types, js, os_config(true));
    host.set_object_script_files(w.files.get());
    host.start_game(ModeKind::Land);
    ObjectScriptHost& os = *host.object_scripts();
    const objects::ObjectStore& st = host.tactical()->primary();
    auto id_of = [&](ObjectHandle h) { return std::to_string(st.id(h)); };
    // Type setup (§1.5): the Os_TypeScript type instance made the Os_Pool4 pool (4 loaded: 1 held by the type, 3 free);
    // Base_Definitions once, Object nil, never serviced.
    const auto type_t = static_cast<objects::TypeIndex>(w.db.find("Os_TypeScript"));
    CHECK(os.type_instances() == 1 && os.type_vm(type_t) != nullptr && os.stats().loaded == 4);
    std::vector<ObjectScriptHost::PoolInfo> pools = os.pools();
    const ObjectScriptHost::PoolInfo* p4 = pool_of(pools, "Os_Pool4");
    CHECK(p4 && p4->size == 4 && p4->in_use == 1);
    CHECK(gv_num(host, "P4BD") == 1.0 && gv_text(host, "P4Obj") == "nil");
    host.run_fast(3);
    CHECK(host.global_values()->get("P4Main").kind == lua::AiGlobalValue::Nil && os.stats().pumps == 0);
    // An object of that type takes a free preloaded instance: still fresh, so Base_Definitions runs at its attach.
    const ObjectHandle ts = spawn(host, w, "Os_TypeScript", 0, {0, 0, 0});
    CHECK(os.stats().loaded == 4 && os.stats().reused == 1 && gv_num(host, "P4BD") == 2.0);
    host.run_fast(1);
    CHECK(gv_text(host, "P4Main") == "0/1/1/" + id_of(ts) + ";");
    const uint64_t loaded0 = os.stats().loaded, reused0 = os.stats().reused;
    // No ScriptPoolCount: a pool of 1 that grows to the peak live count. A released instance runs Flush_G and
    // Base_Definitions at once; its reuse runs neither the top level nor Base_Definitions; thread ids restart at 0;
    // numbers survive Flush_G.
    const ObjectHandle a = spawn(host, w, "Os_Pool", 0, {0, 0, 0});
    pools = os.pools();
    const ObjectScriptHost::PoolInfo* p = pool_of(pools, "Os_Pool");
    CHECK(os.stats().loaded == loaded0 + 1 && gv_num(host, "PBD") == 1.0 && p && p->size == 1 && p->in_use == 1);
    host.run_fast(1);
    const std::string a_id = id_of(a);  // before the destroy (the handle goes stale)
    host.tactical()->destroy(a);
    host.run_fast(1);
    pools = os.pools();
    p = pool_of(pools, "Os_Pool");
    CHECK(p && p->size == 1 && p->in_use == 0 && gv_num(host, "PFlush") == 1.0 && gv_num(host, "PBD") == 2.0 &&
          os.stats().pooled_releases == 1 && os.stats().released_dead == 1);
    const ObjectHandle b = spawn(host, w, "Os_Pool", 0, {0, 0, 0});
    CHECK(os.stats().loaded == loaded0 + 1 && os.stats().reused == reused0 + 1 && gv_num(host, "PBD") == 2.0);
    host.run_fast(1);
    CHECK(gv_text(host, "PMain") == "0/1/1/" + a_id + ";0/2/1/" + id_of(b) + ";");
    spawn(host, w, "Os_Pool", 0, {0, 0, 0});  // none free: one more
    pools = os.pools();
    p = pool_of(pools, "Os_Pool");
    CHECK(p && p->size == 2 && p->in_use == 2 && os.stats().loaded == loaded0 + 2 && gv_num(host, "PBD") == 3.0);
    // ScriptPoolCount 3: three loaded at the first attach, the next two attaches reuse, the fourth loads one more.
    const uint64_t loaded1 = os.stats().loaded;
    for (int i = 0; i < 3; ++i) spawn(host, w, "Os_Pool3", 0, {0, 0, 0});
    CHECK(os.stats().loaded == loaded1 + 3);
    spawn(host, w, "Os_Pool3", 0, {0, 0, 0});
    pools = os.pools();
    const ObjectScriptHost::PoolInfo* p3 = pool_of(pools, "Os_Pool3");
    CHECK(os.stats().loaded == loaded1 + 4 && p3 && p3->size == 4 && p3->in_use == 4);
    // ScriptPoolCount 0: no pool; every attach loads, a release closes the VM (no Flush_G).
    const uint64_t loaded2 = os.stats().loaded;
    const ObjectHandle z = spawn(host, w, "Os_Pool0", 0, {0, 0, 0});
    host.run_fast(1);
    host.tactical()->destroy(z);
    host.run_fast(1);
    spawn(host, w, "Os_Pool0", 0, {0, 0, 0});
    pools = os.pools();
    CHECK(os.stats().loaded == loaded2 + 2 && pool_of(pools, "Os_Pool0") == nullptr &&
          host.global_values()->get("P0Flush").kind == lua::AiGlobalValue::Nil);
    // Mode end: every pooled instance goes back to its pool (Flush_G + Base_Definitions); the type instance stays.
    const double flushes = gv_num(host, "PFlush");
    CHECK(host.end_game_mode());
    CHECK(gv_num(host, "PFlush") == flushes + 2.0 && os.live() == 0);
    for (const ObjectScriptHost::PoolInfo& q : os.pools()) CHECK(q.in_use == (q.name == "Os_Pool4" ? 1u : 0u));
    std::printf("object scripts pool: loaded %llu, reused %llu, pooled releases %llu\n",
                static_cast<unsigned long long>(os.stats().loaded), static_cast<unsigned long long>(os.stats().reused),
                static_cast<unsigned long long>(os.stats().pooled_releases));
}

// q6 t19 (§2.6, LUA_SAVE): object scripts in a save. A bare galactic session plus a battle glue participant that pushes a
// Land mode at load (two-pass load, as in test_save_session); scripts in both stores, mid-thread (Sleep, a waiting
// thread), pooled (held and free instances, a pooled release before the save), unpooled (ScriptPoolCount 0), the type
// instance. The load runs nothing of the scripts (no top level, Base_Definitions, main or Object: SaveOnEnter, TopRuns
// and the pool counters unchanged) and the run continues with the uninterrupted run's CRCs (GlobalValue writes are in
// the frame CRC, first-pump draws in the rng CRC).
static void test_save_load(const OsWorld& w) {
    struct Glue {
        SimHost* host = nullptr;
        bool resume = false;
        int rebuilds = 0;
    };
    auto make = [&w](jobs::JobSystem& js, std::shared_ptr<Glue>& g) {
        auto h = std::make_unique<SimHost>(w.types, js, os_config(true));
        h->set_object_script_files(w.files.get());
        h->start_game(ModeKind::Galactic);
        g = std::make_shared<Glue>();
        g->host = h.get();
        SaveParticipant p;
        p.name = "test.battle";
        p.write = [g](ByteWriter& out) { out.boolean(g->host->modes().depth() > 1); };
        p.read = [g](ByteReader& in) {
            const bool tactical = in.boolean();
            if (!in.ok()) return false;
            g->resume = tactical && g->host->modes().depth() == 1;
            return true;
        };
        p.rebuild = [g] {
            if (!g->resume) return std::string();
            g->resume = false;
            ++g->rebuilds;
            g->host->start_game(ModeKind::Land);
            return std::string();
        };
        h->add_save_participant(std::move(p));
        return h;
    };
    auto bottom = [](SimHost& h) -> objects::ObjectStore& { return h.modes().at(h.modes().depth() - 1).primary(); };
    auto create = [&w](objects::ObjectStore& st, const char* type) {
        objects::SpawnParams sp;
        sp.type = static_cast<objects::TypeIndex>(w.db.find(type));
        sp.owner = 0;
        sp.facing = {1.0f, 0.0f, 0.0f};
        return st.create(sp);
    };
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    std::shared_ptr<Glue> ga, gb;
    auto a = make(js, ga);
    ObjectScriptHost& osa = *a->object_scripts();
    const ObjectHandle gal_save = create(bottom(*a), "Os_Save");
    create(bottom(*a), "Os_Gate");
    a->run_fast(20);
    a->start_game(ModeKind::Land);
    std::vector<ObjectHandle> land;
    for (const char* t : {"Os_Save", "Os_Save", "Os_Pool", "Os_Pool", "Os_Pool0", "Os_TypeScript", "Os_Prox"})
        land.push_back(spawn(*a, w, t, 0, {static_cast<float>(10 * land.size()), 0, 0}));
    spawn(*a, w, "Os_Plain", 1, {75, 10, 0});
    a->run_fast(25);
    a->tactical()->destroy(land[2]);  // a pooled release: Flush_G + Base_Definitions, the instance waits free
    a->run_fast(6);
    std::string why;
    CHECK(osa.live() == 8 && a->save_allowed(&why));
    SaveFile save;
    CHECK(a->save(save, &why) && save.find("object_scripts") != nullptr);
    if (!why.empty()) std::printf("  object script save: %s\n", why.c_str());
    const ObjectScriptHost::Stats at_save = osa.stats();
    const std::vector<ObjectScriptHost::PoolInfo> pools_at_save = osa.pools();
    const double on_enter = gv_num(*a, "SaveOnEnter"), pbd = gv_num(*a, "PBD"), p4bd = gv_num(*a, "P4BD");
    CHECK(on_enter == 3.0 && at_save.pooled_releases == 1);
    a->run_fast(150);
    CrcLog log_a;
    for (const CrcRecord& rec : a->crc_log().records)
        if (rec.tick >= save.tick) log_a.records.push_back(rec);
    log_a.systems = a->crc_log().systems;
    log_a.parity_parts = a->crc_log().parity_parts;

    auto b = make(js, gb);
    LoadReport rep;
    CHECK(b->load(save, &why, &rep));
    if (!rep.ok()) std::printf("  object script load: %s\n", rep.text().c_str());
    ObjectScriptHost& osb = *b->object_scripts();
    CHECK(b->modes().depth() == 2 && gb->rebuilds == 1 && osb.live() == 8 && osb.type_instances() == 1);
    const ObjectScriptHost::Stats s = osb.stats();
    CHECK(s.attached == at_save.attached && s.loaded == at_save.loaded && s.reused == at_save.reused &&
          s.pumps == at_save.pumps && s.first_draws == at_save.first_draws && s.pooled_releases == at_save.pooled_releases);
    const std::vector<ObjectScriptHost::PoolInfo> pools_b = osb.pools();
    CHECK(pools_b.size() == pools_at_save.size());
    for (size_t i = 0; i < pools_b.size() && i < pools_at_save.size(); ++i)
        CHECK(pools_b[i].name == pools_at_save[i].name && pools_b[i].size == pools_at_save[i].size &&
              pools_b[i].in_use == pools_at_save[i].in_use);
    CHECK(osb.has_script(bottom(*b), gal_save) && osb.has_script(b->tactical()->primary(), land[0]) &&
          !osb.has_script(b->tactical()->primary(), land[2]));
    CHECK(b->run_fast(150) == 150);
    const CrcDivergence dv = first_divergence(log_a, b->crc_log());
    if (dv.diverged) std::printf("  object script save: %s\n", dv.message.c_str());
    CHECK(!dv.diverged);
    // Nothing re-ran at the load (the OnEnter count, the top levels, the pooled Base_Definitions), the threads went on.
    CHECK(gv_num(*b, "SaveOnEnter") == on_enter && gv_num(*a, "SaveOnEnter") == on_enter && gv_num(*b, "PBD") == pbd &&
          gv_num(*b, "P4BD") == p4bd);
    const std::string key = "Save" + std::to_string(bottom(*a).id(gal_save));
    CHECK(gv_text(*b, key.c_str()) == gv_text(*a, key.c_str()) && gv_text(*b, key.c_str()).find("/true/true/true/1") != std::string::npos);
    CHECK(gv_text(*b, "Gate") == gv_text(*a, "Gate") && gv_text(*b, "Hits") == gv_text(*a, "Hits") && !gv_text(*a, "Hits").empty());
    CHECK(osb.stats().pumps == osa.stats().pumps && osb.stats().first_draws == osa.stats().first_draws);
    std::printf("object scripts save: %zu chunks, %zu live scripts restored at tick %" PRIu64 ", 150 ticks %s, %s\n",
                save.chunks().size(), osb.live(), save.tick, dv.diverged ? "DIVERGED" : "identical",
                gv_text(*b, key.c_str()).c_str());
}

static void test_object_scripts() {
    const auto w = make_os_world();
    test_attach_and_gate(*w);
    test_release(*w);
    test_proximity_and_spawner(*w);
    test_pool(*w);
    test_worker_counts(*w);
    test_save_load(*w);
    std::error_code ec;
    remove_all_long(w->root, ec);
    std::printf("object scripts: attach, gate, release, proximity, spawner, worker counts checked\n");
}

}  // namespace object_script_test

static void test_story_synthetic(const World& w) {
    using namespace story_test;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Host_Test.xml", kPlots);
    write_file(root / "Data/XML/Story_Host_Test.xml", kPlot);
    write_file(root / "Data/Scripts/Story/Story_Host_Test.lua", kScript);
    DirectoryProvider files(path_to_utf8(root));
    const uint64_t ticks = 600;
    const Run a = run(w, files, 0, ticks);
    std::printf("story synthetic: %zu fired, Empire tech %d, credits %.0f, final CRC %016" PRIx64 "\n", a.fired, a.empire_tech,
                static_cast<double>(a.empire_credits), a.crc.records.empty() ? 0 : a.crc.records.back().combined);
    // Every mapped trigger fired once, in a documented order (golden), the host applied SET_TECH_LEVEL and CREDITS.
    auto fired = [&a](const char* ev) { return a.log.find(std::string(" ") + ev + " ") != std::string::npos; };
    for (const char* ev : {"Begin", "Raise_Tech", "Tech_Seen", "Script_Ack", "Ack_Flag", "Enemy_Takes_C", "Corvette_At_C",
                           "Landed_C", "C_Corrupted", "Two_Fighters", "Bonus", "Two_Seconds"})
        if (!fired(ev)) {
            std::printf("FAIL story event %s did not fire\n", ev);
            ++g_fail;
        }
    CHECK(!fired("Friendly_Takes_C"));
    CHECK(a.empire_tech == 3);
    CHECK(a.log.find("TacticalStarted") != std::string::npos && a.log.find("TacticalEnded") != std::string::npos);
    CHECK(a.crc.records.size() == ticks + 1);
    CHECK(!a.crc.records.empty() && a.crc.records.front().systems.size() == 8);  // + "story"
    for (int32_t workers : jobs::determinism_worker_counts()) {
        if (workers == 0) continue;
        const Run b = run(w, files, workers, ticks);
        const auto d = first_divergence(a.crc, b.crc);
        std::printf("  story workers %2d: %s, log %s\n", workers, d.message.c_str(), b.log == a.log ? "identical" : "DIFFERS");
        CHECK(!d.diverged && b.log == a.log);
    }
    check_text_golden("story_synthetic_golden.txt", a.log);
}

// --- OI lua: story rewards LOCK_UNIT (GalacticSim::lock_type for the plot's player) and REMOVE_CORRUPTION; REMAINING_GAPS
// 1.4-1.5: BUILDABLE_UNIT (P1 unlock, P2 lock), ENABLE/DISABLE_BUILDABLE (production-disabled list), SET_TECH_LEVEL ---
static void test_story_rewards_oi(const World& w) {
    using namespace story_test;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_oi";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Oi.xml",
               "<Story_Mode_Plots><Active_Plot>Story_Oi.xml</Active_Plot><Suspended_Plot /></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Oi.xml",
               "<Story>\n"
               " <Event Name=\"Begin\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n"
               " <Event Name=\"Lock\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>LOCK_UNIT</Reward_Type>"
               "<Reward_Param1>Corvette</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Lock_Bad\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>LOCK_UNIT</Reward_Type>"
               "<Reward_Param1>No_Such_Unit</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Clean_C\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REMOVE_CORRUPTION</Reward_Type>"
               "<Reward_Param1>C</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Clean_Bad\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REMOVE_CORRUPTION</Reward_Type>"
               "<Reward_Param1>Nowhere</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Lock_F\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>LOCK_UNIT</Reward_Type>"
               "<Reward_Param1>Fighters</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Unlock_F\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>BUILDABLE_UNIT</Reward_Type>"
               "<Reward_Param1>Fighters</Reward_Param1><Reward_Param2>Barracks</Reward_Param2><Prereq>Lock_F</Prereq></Event>\n"
               " <Event Name=\"Enable_I\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>ENABLE_BUILDABLE</Reward_Type>"
               "<Reward_Param1>Infantry</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Disable_I\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>DISABLE_BUILDABLE</Reward_Type>"
               "<Reward_Param1>Infantry</Reward_Param1><Prereq>Enable_I</Prereq></Event>\n"
               " <Event Name=\"Disable_F\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>DISABLE_BUILDABLE</Reward_Type>"
               "<Reward_Param1>Fighters</Reward_Param1><Prereq>Unlock_F</Prereq></Event>\n"
               " <Event Name=\"Enable_F\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>ENABLE_BUILDABLE</Reward_Type>"
               "<Reward_Param1>Fighters</Reward_Param1><Prereq>Disable_F</Prereq></Event>\n"
               " <Event Name=\"Max_2\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_MAX_TECH_LEVEL</Reward_Type>"
               "<Reward_Param1>Empire</Reward_Param1><Reward_Param2>2</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Tech_4\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_TECH_LEVEL</Reward_Type>"
               "<Reward_Param1>Empire</Reward_Param1><Reward_Param2>4</Reward_Param2><Prereq>Max_2</Prereq></Event>\n"
               // GRC 1.5 reveal rewards: REVEAL_PLANET needs both params (P2 0 clears), ENABLE_GALACTIC_REVEAL needs P1.
               " <Event Name=\"Hide_B\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REVEAL_PLANET</Reward_Type>"
               "<Reward_Param1>PlanetB</Reward_Param1><Reward_Param2>0</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Reveal_Noop\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REVEAL_PLANET</Reward_Type>"
               "<Reward_Param1>PlanetD</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"No_Auto\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>ENABLE_GALACTIC_REVEAL</Reward_Type>"
               "<Reward_Param1>0</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"No_Auto_Bad\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>ENABLE_GALACTIC_REVEAL</Reward_Type>"
               "<Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Later\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>5</Event_Param1><Prereq>Hide_B</Prereq></Event>\n"
               " <Event Name=\"Reveal_All\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>REVEAL_ALL_PLANETS</Reward_Type>"
               "<Prereq>Later</Prereq></Event>\n"
               // GRC 2.3 SET_PLANET_VISIBILITY_LEVEL: P1 planet, P2 faction, P3 level / "none", P4 key, P5 seconds (missing = never).
               " <Event Name=\"Vis_Max\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_PLANET_VISIBILITY_LEVEL</Reward_Type>"
               "<Reward_Param1>PlanetD</Reward_Param1><Reward_Param2>Empire</Reward_Param2><Reward_Param3>Maximum</Reward_Param3>"
               "<Reward_Param4>Spy_D</Reward_Param4><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Vis_Short\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_PLANET_VISIBILITY_LEVEL</Reward_Type>"
               "<Reward_Param1>PlanetC</Reward_Param1><Reward_Param2>Empire</Reward_Param2><Reward_Param3>Fleet_Presence</Reward_Param3>"
               "<Reward_Param4>Spy_C</Reward_Param4><Reward_Param5>0.5</Reward_Param5><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Vis_Bad\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_PLANET_VISIBILITY_LEVEL</Reward_Type>"
               "<Reward_Param1>Nowhere</Reward_Param1><Reward_Param2>Empire</Reward_Param2><Reward_Param3>Maximum</Reward_Param3>"
               "<Reward_Param4>K</Reward_Param4><Prereq>Begin</Prereq></Event>\n"
               // L1P4_LUA_GAPS 3 SABOTAGE_STRUCTURE: P1 = planet TYPE name (case-insensitive; the map name "A" is not
               // matched), P2 = structure type; both required.
               " <Event Name=\"Sab_Map\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SABOTAGE_STRUCTURE</Reward_Type>"
               "<Reward_Param1>A</Reward_Param1><Reward_Param2>Barracks</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Sab_One\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SABOTAGE_STRUCTURE</Reward_Type>"
               "<Reward_Param1>PlanetA</Reward_Param1><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Sab_None\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SABOTAGE_STRUCTURE</Reward_Type>"
               "<Reward_Param1>PlanetB</Reward_Param1><Reward_Param2>Barracks</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
               " <Event Name=\"Sab_Ok\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SABOTAGE_STRUCTURE</Reward_Type>"
               "<Reward_Param1>planeta</Reward_Param1><Reward_Param2>Barracks</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
               "</Story>\n");
    DirectoryProvider files(path_to_utf8(root));
    std::string first_log;
    auto barracks_at_a = [](const GalacticSim& g) {
        size_t n = 0;
        for (const ObjectHandle s : g.planets()[rA].structures) n += g.store().alive(s) && g.store().type(s) == tBarracks ? 1u : 0u;
        return n;
    };
    for (int32_t workers : {0, 4}) {
        GalacticRules r = rules();
        r.types[tInfantry].build_initially_locked = true;
        // SET_TECH_LEVEL 4 sweep (REMAINING_GAPS 1.4): SB1 (sliceable, Tech_Level 3 < 4) unlocks, Infantry (Tech_Level 4)
        // does not.
        r.types[tInfantry].sliceable = true;
        r.types[tInfantry].tech_level = 4;
        r.types[tSB1].build_initially_locked = r.types[tSB1].sliceable = true;
        r.types[tSB1].tech_level = 3;
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 7;
        cfg.story_crc = true;
        SimHost host(w.types, js, cfg);
        GalacticSim* gs = host.start_galactic(r, setup());
        CHECK(gs != nullptr);
        if (!gs) return;
        gs->set_corruption(rC, kCorruptionRacketeering, pUnder);
        CHECK(!gs->type_unlocked(pEmpire, tInfantry) && !gs->type_unlocked(pEmpire, tSB1));
        CHECK(gs->corrupted(rC) && !gs->type_locked(pEmpire, tCorvette));
        CHECK(gs->max_tech_level(pEmpire) == 5 && gs->players()[pEmpire].tech_level < 4);
        CHECK(barracks_at_a(*gs) == 1);
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pEmpire, "Story_Plots_Oi.xml"}};
        host.tick_once();  // the galactic pre-step: rA (held) and its neighbours revealed (auto-reveal on)
        const bool d_before = gs->planet_revealed_raw(rD);
        CHECK(gs->planet_auto_reveal() && gs->planet_revealed_raw(rA) && gs->planet_revealed_raw(rB));
        CHECK(host.start_story(st, nullptr) && host.story());
        if (!host.story()) return;
        bool removing = false;
        for (int t = 0; t < 40; ++t) {
            host.tick_once();
            removing = removing || gs->corruption_transition_active(rC);
        }
        CHECK(gs->type_locked(pEmpire, tCorvette) && !gs->type_locked(pUnder, tCorvette));
        CHECK(gs->build_locked(pEmpire, tCorvette));
        // REVEAL_PLANET PlanetB 0 cleared rB's byte, REVEAL_PLANET PlanetD without P2 is a no-op, ENABLE_GALACTIC_REVEAL 0
        // turned auto-reveal off (no P1: logged); the pre-step does not set rB again (not held, no orbit presence, no spread).
        std::printf("  oi reveal: B 1 -> %d, D %d -> %d, auto-reveal %d\n", gs->planet_revealed_raw(rB) ? 1 : 0, d_before ? 1 : 0,
                    gs->planet_revealed_raw(rD) ? 1 : 0, gs->planet_auto_reveal() ? 1 : 0);
        CHECK(!gs->planet_revealed_raw(rB) && gs->planet_revealed_raw(rD) == d_before && !gs->planet_auto_reveal());
        // REMOVE_CORRUPTION without P2: the timed removal over 10 s (GRC 4.3), completed by the planet service.
        CHECK(removing || !gs->corrupted(rC));
        for (int t = 0; t < 4000 && gs->corrupted(rC); ++t) host.tick_once();
        CHECK(!gs->corrupted(rC) && !gs->corruption_transition_active(rC));
        // REVEAL_ALL_PLANETS (5 s after Hide_B) set every byte.
        CHECK(gs->planet_revealed_raw(rA) && gs->planet_revealed_raw(rB) && gs->planet_revealed_raw(rC) && gs->planet_revealed_raw(rD));
        // BUILDABLE_UNIT: P1 leaves the locked list and joins the unlocked list, P2 joins the locked list (Empire is not
        // the Good_Side_Name faction: no tech raise).
        CHECK(!gs->type_locked(pEmpire, tFighters) && gs->type_unlocked(pEmpire, tFighters) && !gs->build_locked(pEmpire, tFighters));
        CHECK(gs->type_locked(pEmpire, tBarracks) && !gs->type_locked(pUnder, tBarracks));
        // ENABLE/DISABLE_BUILDABLE edit the production-disabled list only: no unlock, no buildability change.
        CHECK(gs->production_disabled(pEmpire, tInfantry) && !gs->production_disabled(pUnder, tInfantry));
        CHECK(!gs->type_unlocked(pEmpire, tInfantry) && !gs->type_locked(pEmpire, tInfantry) && gs->build_locked(pEmpire, tInfantry));
        CHECK(!gs->production_disabled(pEmpire, tFighters) && gs->type_unlocked(pEmpire, tFighters));
        // SET_TECH_LEVEL 4 above max tech 2 raises the max first, then sets the tech with the sweep.
        CHECK(gs->max_tech_level(pEmpire) == 4 && gs->players()[pEmpire].tech_level == 4);
        CHECK(gs->type_unlocked(pEmpire, tSB1) && !gs->build_locked(pEmpire, tSB1) && !gs->type_unlocked(pUnder, tSB1));
        // SET_PLANET_VISIBILITY_LEVEL: rD keeps the Maximum entry (never expires); rC's 0.5 s entry has expired.
        {
            const auto d_mods = gs->visibility_modifiers(rD, pEmpire);
            CHECK(d_mods.size() == 1 && d_mods[0].value == galactic::kVisMaximum && d_mods[0].expiry == galactic::kVisNever);
            CHECK(gs->visibility_mask(rD, pEmpire) == galactic::kVisMaximum);
            CHECK(gs->visibility_modifiers(rC, pEmpire).empty());
        }
        const auto& msgs = host.story()->lua_messages();
        size_t lock_err = 0, clean_err = 0, vis_err = 0;
        for (const auto& m : msgs)
            vis_err += m.find("SET_PLANET_VISIBILITY_LEVEL failed due to unknown planet 'Nowhere'") != std::string::npos ? 1u : 0u;
        CHECK(vis_err == 1);
        for (const auto& m : msgs) {
            lock_err += m.find("LOCK_UNIT failed due to unknown unit type 'No_Such_Unit'") != std::string::npos ? 1u : 0u;
            clean_err += m.find("REMOVE_CORRUPTION failed due to unknown planet 'Nowhere'") != std::string::npos ? 1u : 0u;
        }
        size_t reveal_err = 0;
        for (const auto& m : msgs) reveal_err += m.find("ENABLE_GALACTIC_REVEAL failed due to missing Param1") != std::string::npos ? 1u : 0u;
        CHECK(lock_err == 1 && clean_err == 1 && reveal_err == 1);
        // SABOTAGE_STRUCTURE: "planeta" destroyed rA's Barracks; the map name "A", the missing P2 and PlanetB (no
        // Barracks) are logged.
        size_t sab_map = 0, sab_one = 0, sab_none = 0;
        for (const auto& m : msgs) {
            sab_map += m.find("SABOTAGE_STRUCTURE failed due to invalid planet A") != std::string::npos ? 1u : 0u;
            sab_one += m.find("SABOTAGE_STRUCTURE takes two parameters") != std::string::npos ? 1u : 0u;
            sab_none += m.find("SABOTAGE_STRUCTURE failed due to non-existent structure Barracks") != std::string::npos ? 1u : 0u;
        }
        CHECK(sab_map == 1 && sab_one == 1 && sab_none == 1 && barracks_at_a(*gs) == 0);
        const std::string log = host.story()->log_text("oi rewards");
        if (first_log.empty()) first_log = log;
        CHECK(log == first_log && log.find("reward=LOCK_UNIT[Corvette]") != std::string::npos);
    }
    remove_all_long(root, ec);
}
// --- end OI lua ---

// --- host follow-up: the T9 time argument. SimHost feeds StoryModel the (float)(galactic mode frame) x (1/30 as float)
// clock (STORY.md 4.7 / 7.3), not the global frame and not wall time: time-based events (STORY_ELAPSED) fire at the
// frame the spec gives, a pushed tactical mode stalls the story clock (the galactic mode frame does not advance),
// and the whole series and the fired frames are identical for 0/1/4/15 workers. ---
static void test_story_t9_clock(const World& w) {
    using namespace story_test;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_t9";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_T9.xml",
               "<Story_Mode_Plots><Active_Plot>Story_T9.xml</Active_Plot><Suspended_Plot /></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_T9.xml",
               "<Story>\n"
               " <Event Name=\"Zero\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n"
               " <Event Name=\"Five\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>5</Event_Param1></Event>\n"
               " <Event Name=\"Half\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>12.5</Event_Param1></Event>\n"
               "</Story>\n");
    DirectoryProvider files(path_to_utf8(root));
    constexpr uint64_t kTicks = 500, kBattleFrom = 100, kBattleTo = 160;  // a space battle on top of the galaxy for 60 ticks
    constexpr float kSpf = 1.0f / 30.0f;
    struct Sample {
        uint64_t global = 0, galactic = 0;  // global frame, galactic mode frame after the tick
        uint64_t seen = 0;                  // the galactic mode frame T9 saw (before the mode's end-of-tick increment)
        double time = 0.0;
    };
    std::vector<Sample> first;
    std::map<std::string, uint64_t> first_fired;  // event -> global frame
    for (int32_t workers : jobs::determinism_worker_counts()) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 7;
        cfg.story_crc = true;
        SimHost host(w.types, js, cfg);
        const GalacticRules r = rules();  // the sim keeps a reference to the rules
        GalacticSim* gs = host.start_galactic(r, setup());
        CHECK(gs != nullptr);
        if (!gs) return;
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pEmpire, "Story_Plots_T9.xml"}};
        CHECK(host.start_story(st, nullptr) && host.story());
        if (!host.story()) return;
        std::vector<Sample> series;
        for (uint64_t t = 1; t <= kTicks; ++t) {
            if (t == kBattleFrom) host.start_game(ModeKind::Space);
            if (t == kBattleTo) host.end_game_mode();
            const GalacticMode* g = host.galactic_mode();
            CHECK(g != nullptr);
            if (!g) return;
            const uint64_t seen = g->frame();
            host.tick_once();
            series.push_back({host.frame(), g->frame(), seen, host.story()->model(0).time()});
        }
        // Every tick the story clock is the T9 time of the galactic mode frame T9 saw (bit-exact, computed in float).
        bool exact = true, differs_from_double = false;
        for (const Sample& s : series) {
            const bool ok = s.time == lua::StoryModel::t9_seconds(s.seen, kSpf) && s.time == StorySession::t9_seconds(s.seen);
            if (!ok && exact) std::printf("  t9 clock mismatch at global %" PRIu64 " seen %" PRIu64 " time %.9g\n", s.global, s.seen, s.time);
            exact = exact && ok;
            differs_from_double = differs_from_double || s.time != static_cast<double>(s.seen) / 30.0;
        }
        CHECK(exact);
        CHECK(differs_from_double);  // the float product is the clock, not mode frame / 30 in double
        // The pushed tactical mode stalls the clock: the global frame ran ahead of the galactic mode frame by the 60 battle ticks.
        CHECK(series.back().global == kTicks && series.back().galactic == kTicks - (kBattleTo - kBattleFrom));
        // Fired frames: Zero at the second T9 (A2i t01: the first evaluation only records LastTime, 0x454980); Five / Half
        // where the accumulated T9 clock reaches 5 s / 12.5 s (replayed here from the recorded series: LastTime = first T9,
        // accumulated += now - LastTime from the second T9 on, fire at accumulated >= Param1).
        std::map<std::string, uint64_t> fired;
        for (const StoryLogEntry& e : host.story()->log()) fired[e.event] = e.frame;
        auto expect = [&series](double threshold) -> uint64_t {
            double last = 0.0, acc = 0.0;
            for (size_t i = 0; i < series.size(); ++i) {
                if (i) acc += series[i].time - last;
                last = series[i].time;
                if (i && acc >= threshold) return series[i].global;
            }
            return 0;
        };
        std::printf("  t9 clock, workers %2d: Zero @%" PRIu64 ", Five @%" PRIu64 ", Half @%" PRIu64 "\n", workers, fired["Zero"],
                    fired["Five"], fired["Half"]);
        CHECK(fired.size() == 3 && fired["Zero"] == 2 && expect(0.0) == 2);
        CHECK(fired["Five"] == expect(5.0) && fired["Half"] == expect(12.5));
        // The spec's frame: 5 s = 150 galactic frames after the first T9 (+-1 for the float rounding of the clock), and the
        // 60 stalled battle ticks delay the global frame (Five lands after the battle, Half too).
        const uint64_t stall = kBattleTo - kBattleFrom;
        CHECK(fired["Five"] >= 151 - 1 + stall && fired["Five"] <= 151 + 1 + stall);
        CHECK(fired["Half"] >= 376 - 1 + stall && fired["Half"] <= 376 + 1 + stall);
        if (first.empty()) {
            first = series;
            first_fired = fired;
        } else {
            bool same = series.size() == first.size() && fired == first_fired;
            for (size_t i = 0; same && i < series.size(); ++i)
                same = series[i].global == first[i].global && series[i].galactic == first[i].galactic && series[i].seen == first[i].seen &&
                       series[i].time == first[i].time;
            CHECK(same);
        }
    }
    remove_all_long(root, ec);
}

// A2i t01 (A2I_T01_FRAME_ORDER.md §6): the load-time story pass. start_story ends with STORY_TECH_LEVEL (each player's
// starting tech level) to that player's plot sets, in player-index order, with the Fire chain applied at once: a
// TECH_LEVEL event's UNIQUE_UNIT reward spawns its unit before tick 1 (in the initial CRC record), its STORY_TRIGGER
// dependant pays at load too, the T9 TECH_LEVEL poll never fires them again, and a prereq-less ELAPSED 0 fires at T9 of
// tick 2 (the timer's first evaluation only records its start, 0x454980). The plot sets are loaded Underworld first so
// the player order (Empire = player 0 first) is visible in the log and in the object ids. Variant "high": the tech
// events need level 9 (never fire), to show the spawn is what moves the initial record.
static void test_story_load_pass(const World& w) {
    using namespace story_test;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_load";
    std::error_code ec;
    remove_all_long(root, ec);
    auto plot = [](const char* tech_event, const char* level, bool elapsed) {
        std::string s = "<Story>\n";
        s += std::string(" <Event Name=\"") + tech_event + "\"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>" + level +
             "</Event_Param1><Reward_Type>UNIQUE_UNIT</Reward_Type><Reward_Param1>Corvette</Reward_Param1></Event>\n";
        s += std::string(" <Event Name=\"") + tech_event + "_Pay\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>CREDITS</Reward_Type>"
             "<Reward_Param1>250</Reward_Param1><Prereq>" + tech_event + "</Prereq></Event>\n";
        if (elapsed) {
            s += " <Event Name=\"Start\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n";
            s += " <Event Name=\"Tech_Two\"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>2</Event_Param1></Event>\n";
        }
        return s + "</Story>\n";
    };
    for (const char* variant : {"", "_High"}) {
        const bool high = *variant != 0;
        const std::string sfx(variant);
        write_file(root / ("Data/XML/Story_Plots_Load_E" + sfx + ".xml"),
                   "<Story_Mode_Plots><Active_Plot>Story_Load_E" + sfx + ".xml</Active_Plot><Suspended_Plot /></Story_Mode_Plots>\n");
        write_file(root / ("Data/XML/Story_Plots_Load_U" + sfx + ".xml"),
                   "<Story_Mode_Plots><Active_Plot>Story_Load_U" + sfx + ".xml</Active_Plot><Suspended_Plot /></Story_Mode_Plots>\n");
        write_file(root / ("Data/XML/Story_Load_E" + sfx + ".xml"), plot("Empire_Tech_1", high ? "9" : "1", true));
        write_file(root / ("Data/XML/Story_Load_U" + sfx + ".xml"), plot("Under_Tech_1", high ? "9" : "1", false));
    }
    DirectoryProvider files(path_to_utf8(root));
    auto corvettes = [](const GalacticSim& g, int32_t owner) {
        std::vector<int32_t> ids;  // ObjectIds, fleet then content order
        for (const FleetState& f : g.fleets())
            if (f.alive && f.owner == owner)
                for (const ObjectHandle u : f.contents)
                    if (g.store().alive(u) && g.store().type(u) == tCorvette) ids.push_back(static_cast<int32_t>(g.store().id(u)));
        return ids;
    };
    struct Out {
        CrcLog crc;
        std::string log;
    };
    constexpr uint64_t kTicks = 8;
    auto run_one = [&](int32_t workers, const std::string& sfx, bool checks) -> Out {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 7;
        cfg.story_crc = true;
        SimHost host(w.types, js, cfg);
        const GalacticRules r = rules();
        GalacticSim* gs = host.start_galactic(r, setup());
        CHECK(gs != nullptr);
        if (!gs) return {};
        const size_t e0 = corvettes(*gs, pEmpire).size(), u0 = corvettes(*gs, pUnder).size();
        const float credits0 = gs->credits(pEmpire);
        CHECK(gs->players()[pEmpire].tech_level == 1 && gs->players()[pUnder].tech_level == 1);
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pUnder, "Story_Plots_Load_U" + sfx + ".xml"}, {pEmpire, "Story_Plots_Load_E" + sfx + ".xml"}};
        std::vector<std::string> diag;
        CHECK(host.start_story(st, &diag) && host.story());
        for (const auto& d : diag) std::printf("  story diag: %s\n", d.c_str());
        if (!host.story()) return {};
        const StorySession& story = *host.story();
        auto count = [&story](const char* ev) {
            size_t n = 0;
            for (const StoryLogEntry& e : story.log()) n += e.event == ev ? 1u : 0u;
            return n;
        };
        auto entry = [&story](const char* ev) -> const StoryLogEntry* {
            for (const StoryLogEntry& e : story.log())
                if (e.event == ev) return &e;
            return nullptr;
        };
        if (checks) {
            // Before tick 1: both spawns, the Empire's pay-out, logged at frame 0 via load; Empire (player 0) first.
            const auto ce = corvettes(*gs, pEmpire), cu = corvettes(*gs, pUnder);
            CHECK(host.frame() == 0 && ce.size() == e0 + 1 && cu.size() == u0 + 1);
            CHECK(!ce.empty() && !cu.empty() && ce.back() < cu.back());
            CHECK(gs->credits(pEmpire) == credits0 + 250.0f);
            CHECK(story.log().size() == 4);
            const StoryLogEntry* et = entry("Empire_Tech_1");
            const StoryLogEntry* ep = entry("Empire_Tech_1_Pay");
            const StoryLogEntry* ut = entry("Under_Tech_1");
            CHECK(et && ep && ut && et->frame == 0 && et->via == "load" && ep->via == "load" && ut->via == "load");
            CHECK(story.log().size() >= 1 && story.log().front().event == "Empire_Tech_1" && story.log().front().plot_set == 1);
            CHECK(count("Start") == 0);
        }
        host.tick_once();
        if (checks) CHECK(count("Start") == 0);  // tick 1's T9: the ELAPSED 0 timer records its start only
        for (uint64_t t = 1; t < kTicks; ++t) host.tick_once();
        if (checks) {
            const StoryLogEntry* s = entry("Start");
            CHECK(s && s->frame == 2 && s->via == "clock" && count("Start") == 1);
            // The T9 TECH_LEVEL poll ran every tick: the load-fired events stay fired (no second spawn, no second pay-out).
            CHECK(count("Empire_Tech_1") == 1 && count("Empire_Tech_1_Pay") == 1 && count("Under_Tech_1") == 1);
            CHECK(count("Tech_Two") == 0 && corvettes(*gs, pEmpire).size() == e0 + 1 && corvettes(*gs, pUnder).size() == u0 + 1);
            CHECK(gs->credits(pEmpire) == credits0 + 250.0f);
        }
        return {host.crc_log(), story.log_text("a2i load pass")};
    };
    const Out a = run_one(0, "", true);
    const Out high = run_one(0, "_High", false);
    // The spawns are in the initial record (captured by the first tick_once before T1): objects and story differ from the
    // run whose tech events never fire.
    CHECK(a.crc.records.size() == kTicks + 1 && high.crc.records.size() == kTicks + 1);
    const auto sys = [&a](const char* name) {
        for (size_t i = 0; i < a.crc.systems.size(); ++i)
            if (a.crc.systems[i] == name) return i;
        return a.crc.systems.size();
    };
    const size_t objects = sys("objects"), story = sys("story");
    CHECK(objects < a.crc.systems.size() && story < a.crc.systems.size());
    if (!a.crc.records.empty() && !high.crc.records.empty() && objects < a.crc.systems.size() && story < a.crc.systems.size()) {
        const CrcRecord& r0 = a.crc.records.front();
        const CrcRecord& h0 = high.crc.records.front();
        CHECK(r0.tick == 0 && h0.tick == 0);
        CHECK(r0.systems[objects] != h0.systems[objects] && r0.systems[story] != h0.systems[story]);
    }
    for (int32_t workers : jobs::determinism_worker_counts()) {
        if (workers == 0) continue;
        const Out b = run_one(workers, "", false);
        const auto d = first_divergence(a.crc, b.crc);
        CHECK(!d.diverged && b.log == a.log);
    }
    std::printf("story load pass: %zu records, log %zu bytes\n", a.crc.records.size(), a.log.size());
    remove_all_long(root, ec);
}

// a2j t05 (A2J_T05_STORY_LOAD.md): the load pass's CONSTRUCT side and its galactic stream. Before the tech pass every
// starting-force object raises STORY_CONSTRUCT (+ CONSTRUCT_LEVEL for a starbase) to every plot set ("via=load:construct").
// a2k t12 (A2K_T07_STORY_LOAD_RE.md): at L8 the planets are still neutral, so only neutral-accepting filters fire
// (SB_Built with FILTER_NEUTRAL_ONLY, not SB_Friendly with the default FRIENDLY_ONLY, not Enemy_Corvette); CONSTRUCT_LEVEL
// compares Param2 with SB1's Base_Level 1 under Param3 SPACE (A_Star_1, not A_Star_2), the Barracks raises none
// (A_Ground_1); the walk is planet by planet in <Locations> order, so the Fighters appended last at A come before the
// Corvette at D. In the tech pass the UNIQUE_UNIT reward's Corvette raises CONSTRUCT inside the reward call (spawn hook,
// judged by the Empire-owned planet), so Own_Corvette fires between Tech_1 and Tech_1's STORY_TRIGGER dependant; that
// dependant's SET_TECH_LEVEL raises TechLevelChanged, routed at load ("T 0 0 ..."), not at T9 of tick 1.
static void test_story_load_construct(const World& w) {
    using namespace story_test;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_load_construct";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/Story_Plots_Load_C.xml",
               "<Story_Mode_Plots><Active_Plot>Story_Load_C.xml</Active_Plot><Suspended_Plot /></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Load_C.xml",
               "<Story>\n"
               " <Event Name=\"SB_Built\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>SB1</Event_Param1>"
               "<Event_Param3>FILTER_NEUTRAL_ONLY</Event_Param3></Event>\n"
               " <Event Name=\"SB_Friendly\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>SB1</Event_Param1></Event>\n"
               " <Event Name=\"A_Star_1\"><Event_Type>STORY_CONSTRUCT_LEVEL</Event_Type><Event_Param1>PlanetA</Event_Param1>"
               "<Event_Param2>1</Event_Param2><Event_Param3>SPACE</Event_Param3><Event_Param4>EMPIRE</Event_Param4></Event>\n"
               " <Event Name=\"A_Star_2\"><Event_Type>STORY_CONSTRUCT_LEVEL</Event_Type><Event_Param1>PlanetA</Event_Param1>"
               "<Event_Param2>2</Event_Param2><Event_Param3>SPACE</Event_Param3></Event>\n"
               " <Event Name=\"A_Ground_1\"><Event_Type>STORY_CONSTRUCT_LEVEL</Event_Type><Event_Param1>PlanetA</Event_Param1>"
               "<Event_Param2>1</Event_Param2><Event_Param3>GROUND</Event_Param3></Event>\n"
               " <Event Name=\"Fighters_Neutral\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Fighters</Event_Param1>"
               "<Event_Param3>FILTER_NEUTRAL_ONLY</Event_Param3></Event>\n"
               " <Event Name=\"Corvette_Neutral\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Corvette</Event_Param1>"
               "<Event_Param3>FILTER_NEUTRAL_ONLY</Event_Param3></Event>\n"
               " <Event Name=\"Enemy_Corvette\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Corvette</Event_Param1>"
               "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
               " <Event Name=\"Tech_1\"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>1</Event_Param1>"
               "<Reward_Type>UNIQUE_UNIT</Reward_Type><Reward_Param1>Corvette</Reward_Param1></Event>\n"
               " <Event Name=\"Own_Corvette\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Corvette</Event_Param1></Event>\n"
               " <Event Name=\"Tech_1_Raise\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_TECH_LEVEL</Reward_Type>"
               "<Reward_Param1>Empire</Reward_Param1><Reward_Param2>2</Reward_Param2><Prereq>Tech_1</Prereq></Event>\n"
               "</Story>\n");
    DirectoryProvider files(path_to_utf8(root));
    struct Out {
        CrcLog crc;
        std::string log;
    };
    auto run_one = [&](int32_t workers, bool checks) -> Out {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 7;
        cfg.story_crc = true;
        SimHost host(w.types, js, cfg);
        const GalacticRules r = rules();
        GalacticSetup su = setup();
        su.forces.push_back({pEmpire, rA, tFighters});  // last entry, first planet: walked before D's Corvette
        GalacticSim* gs = host.start_galactic(r, su);
        CHECK(gs != nullptr);
        if (!gs) return {};
        StoryStart st;
        st.files = &files;
        st.plot_sets = {{pEmpire, "Story_Plots_Load_C.xml"}};
        std::vector<std::string> diag;
        CHECK(host.start_story(st, &diag) && host.story());
        for (const auto& d : diag) std::printf("  story diag: %s\n", d.c_str());
        if (!host.story()) return {};
        const StorySession& story = *host.story();
        auto entry = [&story](const char* ev) -> const StoryLogEntry* {
            for (const StoryLogEntry& e : story.log())
                if (e.event == ev) return &e;
            return nullptr;
        };
        if (checks) {
            for (const StoryLogEntry& e : story.log()) std::printf("  load: %s\n", e.text().c_str());
            const StoryLogEntry* sb = entry("SB_Built");
            const StoryLogEntry* a1 = entry("A_Star_1");
            const StoryLogEntry* fn = entry("Fighters_Neutral");
            const StoryLogEntry* cn = entry("Corvette_Neutral");
            const StoryLogEntry* t1 = entry("Tech_1");
            const StoryLogEntry* oc = entry("Own_Corvette");
            const StoryLogEntry* tr = entry("Tech_1_Raise");
            CHECK(host.frame() == 0 && story.log().size() == 7);
            CHECK(!entry("SB_Friendly") && !entry("A_Star_2") && !entry("A_Ground_1") && !entry("Enemy_Corvette"));
            CHECK(sb && a1 && fn && cn && t1 && oc && tr);
            if (sb && a1 && fn && cn && t1 && oc && tr) {
                for (const StoryLogEntry* e : {sb, a1, fn, cn}) CHECK(e->frame == 0 && e->via == "load:construct" && e->sequence < t1->sequence);
                CHECK(sb->sequence < fn->sequence && fn->sequence < cn->sequence);  // planet A before planet D
                CHECK(t1->via == "load" && oc->via == "load" && tr->via == "load");
                CHECK(t1->sequence < oc->sequence && oc->sequence < tr->sequence);  // CONSTRUCT inside the reward call
            }
            // The dependant's tech raise is routed inside the load pass: trigger 0 at frame 0.
            CHECK(gs->players()[pEmpire].tech_level == 2);
            CHECK(story.triggers().size() == 1 && story.triggers()[0].frame == 0 && story.triggers()[0].kind == "TechLevelChanged");
            CHECK(story.log_text().find("\nT 0 0 TechLevelChanged player=Empire value=2 gframe=0\n") != std::string::npos);
        }
        for (int t = 0; t < 4; ++t) host.tick_once();
        if (checks) {
            CHECK(story.log().size() == 7 && story.triggers().size() == 1);  // nothing fires twice; no second TechLevelChanged
        }
        return {host.crc_log(), story.log_text("a2j load construct")};
    };
    const Out a = run_one(0, true);
    for (int32_t workers : jobs::determinism_worker_counts()) {
        if (workers == 0) continue;
        const Out b = run_one(workers, false);
        const auto d = first_divergence(a.crc, b.crc);
        CHECK(!d.diverged && b.log == a.log);
    }
    std::printf("story load construct: %zu records, log %zu bytes\n", a.crc.records.size(), a.log.size());
    remove_all_long(root, ec);
}

// L1/S4 wave 1: DEPLOY / DEFEAT_HERO (company members), PLANET_DESTROYED (poll), BEGIN_ERA (+ dialog
// record), every notify_ui kind (player-scoped), and a story script's Change_Owner / Get_Parent_Object
// through GalacticSim. Own type database in the rules' index order (so Company_Units / Is_Named_Hero
// resolve), no golden: every event must fire once, identical logs for 0/1/4/15 workers.
namespace story_wave1 {
using namespace alamo::galactic;
using story_test::pEmpire;
using story_test::pUnder;
enum : uint32_t { tHeroTeam = story_test::tCount, tHeroUnit, tCount2 };

static const char* kPlot =
    "<Story>\n"
    " <Event Name=\"Deploy_C\"><Event_Type>STORY_DEPLOY</Event_Type><Event_Param1>Hero_Unit</Event_Param1>"
    "<Event_Param2>PlanetC</Event_Param2></Event>\n"
    " <Event Name=\"Hero_Lost\"><Event_Type>STORY_DEFEAT_HERO</Event_Type><Event_Param1>Hero_Unit</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Select_Infantry\"><Event_Type>STORY_SELECT_UNIT</Event_Type><Event_Param1>Infantry</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Select_A\"><Event_Type>STORY_SELECT_PLANET</Event_Type><Event_Param1>PlanetA</Event_Param1></Event>\n"
    " <Event Name=\"Zoom_B\"><Event_Type>STORY_ZOOM_INTO_PLANET</Event_Type><Event_Param1>PlanetB</Event_Param1></Event>\n"
    " <Event Name=\"Zoom_Out\"><Event_Type>STORY_ZOOM_OUT_PLANET</Event_Type></Event>\n"
    " <Event Name=\"Gui\"><Event_Type>STORY_CLICK_GUI</Event_Type><Event_Param1>Button_X</Event_Param1></Event>\n"
    " <Event Name=\"Speech\"><Event_Type>STORY_SPEECH_DONE</Event_Type><Event_Param1>Speech_1</Event_Param1></Event>\n"
    " <Event Name=\"Movie\"><Event_Type>STORY_MOVIE_DONE</Event_Type></Event>\n"
    " <Event Name=\"Generic\"><Event_Type>STORY_GENERIC</Event_Type><Event_Param1>battle_end_closed</Event_Param1></Event>\n"
    " <Event Name=\"Lost\"><Event_Type>STORY_MISSION_LOST</Event_Type></Event>\n"
    " <Event Name=\"Bought\"><Event_Type>STORY_BUY_BLACK_MARKET</Event_Type><Event_Param1>Disruptor_Tech</Event_Param1></Event>\n"
    " <Event Name=\"Sabotage\"><Event_Type>STORY_GALACTIC_SABOTAGE</Event_Type><Event_Param1>SB1</Event_Param1></Event>\n"
    " <Event Name=\"Open_C\"><Event_Type>STORY_OPEN_CORRUPTION</Event_Type><Event_Param1>PlanetC</Event_Param1></Event>\n"
    " <Event Name=\"Destroyed_B\"><Event_Type>STORY_PLANET_DESTROYED</Event_Type><Event_Param1>PlanetB</Event_Param1></Event>\n"
    " <Event Name=\"Era\"><Event_Type>STORY_BEGIN_ERA</Event_Type><Event_Param1>1</Event_Param1>"
    "<Story_Dialog>Dialog_Test</Story_Dialog><Story_Chapter>2</Story_Chapter><Story_Tag>Era_Tag</Story_Tag>"
    "<Story_Dialog_Popup>False</Story_Dialog_Popup></Event>\n"
    " <Event Name=\"Owner_Done\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>OWNER_DONE</Event_Param2></Event>\n"
    " <Event Name=\"Won_Two_At_C\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>2</Event_Param1>"
    "<Event_Param2>2</Event_Param2><Event_Param4>PlanetC</Event_Param4></Event>\n"
    " <Event Name=\"Lost_One\"><Event_Type>STORY_LOSE_BATTLES</Event_Type><Event_Param1>1</Event_Param1></Event>\n"
    " <Event Name=\"Retreat_C\"><Event_Type>STORY_RETREAT</Event_Type><Event_Param1>PlanetC</Event_Param1></Event>\n"
    "</Story>\n";
// The script moves the Underworld corvette to the Empire (GalacticSim::change_owner: a new Empire fleet in
// orbit) and reports when the owner and the parent fleet changed.
static const char* kScript =
    "function Base_Definitions() ServiceRate = 0.1 done = false end\n"
    "function main() while true do\n"
    "  if not done then local c = Find_First_Object('Corvette') local f = c and c.Get_Parent_Object()\n"
    "    if f then c.Change_Owner(Find_Player('Empire')) local g = c.Get_Parent_Object()\n"
    "      if c.Get_Owner() == Find_Player('Empire') and g and g ~= f and g.Get_Owner() == c.Get_Owner() then\n"
    "        Story_Event('OWNER_DONE') end\n"
    "      done = true end end\n"
    "  coroutine.yield(true) end end\n";

struct Types {
    std::unique_ptr<DirectoryProvider> files;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
};
static std::unique_ptr<Types> make_types(const fs::path& root) {
    // Same order as story_test::rules() + Hero_Team / Hero_Unit.
    write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>Wave1.xml</File></GameObjectFiles>\n");
    write_file(root / "Data/XML/Wave1.xml",
               "<W>\n"
               " <Planet Name=\"PlanetA\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetB\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetC\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetD\"><Behavior>PLANET</Behavior></Planet>\n"
               " <StarBase Name=\"SB1\"/>\n"
               " <SpaceUnit Name=\"Corvette\"/>\n"
               " <GroundInfantry Name=\"Infantry\"/>\n"
               " <GroundStructure Name=\"Barracks\"/>\n"
               " <SpaceUnit Name=\"Fighters\"/>\n"
               " <GroundCompany Name=\"Hero_Team\"><Company_Units>Hero_Unit</Company_Units></GroundCompany>\n"
               " <GroundInfantry Name=\"Hero_Unit\"><Victory_Relevant>Yes</Victory_Relevant><Is_Named_Hero>Yes</Is_Named_Hero></GroundInfantry>\n"
               "</W>\n");
    auto t = std::make_unique<Types>();
    t->files = std::make_unique<DirectoryProvider>(path_to_utf8(root));
    CHECK(t->db.load(*t->files));
    CHECK(t->types.build(t->db));
    CHECK(t->db.find("Hero_Team") == tHeroTeam && t->db.find("Hero_Unit") == tHeroUnit && t->db.find("Corvette") == story_test::tCorvette);
    return t;
}

static GalacticRules rules() {
    GalacticRules r = story_test::rules();
    r.types.resize(tCount2);
    r.types[tHeroTeam].name = "Hero_Team";
    r.types[tHeroTeam].kind = UnitKind::GroundUnit;
    r.types[tHeroUnit].name = "Hero_Unit";
    r.types[tHeroUnit].kind = UnitKind::GroundUnit;
    // P4 lua: one black-market item (BUY_BLACK_MARKET comes from the host's click-time UI notification, not from it).
    BlackMarketItemDef item;
    item.name = "Disruptor_Tech";
    item.name_crc = special_name_crc(item.name);
    r.special.items.push_back(item);
    return r;
}

struct Result {
    std::string log;
    std::vector<std::string> lua;
    size_t dialogs = 0;
    std::string dialog_tag;
    int32_t dialog_chapter = -1;
    bool scoped_ok = false;
    bool battles_ok = false;
    bool special_ok = false;  // P4 lua
    size_t holocron = 0;      // q6 t14
    bool holocron_saved = false;
};

static Result run(const objects::GameObjectTypes& types, const FileProvider& files, int32_t workers) {
    const GalacticRules r = rules();
    GalacticSetup setup = story_test::setup();
    setup.forces.push_back({pUnder, story_test::rD, tHeroTeam});
    setup.forces.push_back({pUnder, story_test::rD, story_test::tCorvette});  // newest: the script's Find_First_Object
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 11;
    cfg.story_crc = true;
    SimHost host(types, js, cfg);
    GalacticSim* gs = host.start_galactic(r, setup);
    CHECK(gs != nullptr);
    if (!gs) return {};
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{pEmpire, "Story_Plots_Wave1.xml"}};
    CHECK(host.start_story(st));
    if (!host.story()) return {};
    StorySession& story = *host.story();
    host.tick_once();  // the script changes the corvette's owner at its first service
    // Underworld: infantry + hero team leave D for C and land; the hero team is destroyed there later.
    const std::vector<ObjectHandle> ground = gs->planets()[story_test::rD].surface;
    CHECK(ground.size() == 2);
    CHECK(gs->launch_units(pUnder, story_test::rD, ground) == RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(story_test::rD, pUnder);
    CHECK(uf.size() == 1);
    if (uf.size() != 1) return {};
    CHECK(gs->move_fleet(pUnder, uf[0], story_test::rC) == RejectReason::None);
    const int32_t infantry_id = gs->store().id(ground[0]);
    Result out;
    bool landed = false, killed = false;
    for (uint64_t t = 1; t < 600; ++t) {
        if (!landed && gs->fleets()[uf[0]].alive && !gs->fleets()[uf[0]].moving && gs->fleets()[uf[0]].planet == story_test::rC) {
            CHECK(gs->invade(pUnder, story_test::rC, {}) == RejectReason::None);
            landed = true;
        } else if (landed && !killed) {
            for (ObjectHandle h : gs->planets()[story_test::rC].surface)
                if (gs->store().alive(h) && gs->store().type(h) == tHeroTeam) killed = gs->destroy_object(h);
        }
        if (t == 10) {
            // Underworld's own click reaches no Empire plot (player-scoped); the Empire's does.
            story.notify_ui({StoryUiEvent::ClickGui, pUnder, 0, "Button_X"});
            story.notify_ui({StoryUiEvent::SelectUnit, pEmpire, infantry_id, ""});
            story.notify_ui({StoryUiEvent::SelectPlanet, pEmpire, gs->store().id(gs->planets()[story_test::rA].object), ""});
            story.notify_ui({StoryUiEvent::ZoomIntoPlanet, pEmpire, 0, "PlanetB"});
            story.notify_ui({StoryUiEvent::ZoomOutPlanet, pEmpire, 0, ""});
            story.notify_ui({StoryUiEvent::SpeechDone, -1, 0, "Speech_1"});
            story.notify_ui({StoryUiEvent::MovieDone, pEmpire, 0, "Any_Movie"});
            story.notify_ui({StoryUiEvent::Generic, pEmpire, 0, "battle_end_closed"});
            story.notify_ui({StoryUiEvent::MissionLost, pEmpire, 0, "M01"});
            // P4 lua (L1P4_LUA_GAPS 2 / 2.5): the GalacticSabotage UI kind is ignored; an executed purchase (AI or human)
            // fires no BUY_BLACK_MARKET (the host raises it at click time); a BuyBlackMarket naming another item fires
            // nothing either.
            story.notify_ui({StoryUiEvent::GalacticSabotage, pEmpire, 0, "SB1"});
            story.notify_ui({StoryUiEvent::BuyBlackMarket, pEmpire, 0, "Cloaking_Tech"});
            story.notify_ui({StoryUiEvent::OpenCorruption, pEmpire, gs->store().id(gs->planets()[story_test::rC].object), ""});
            for (const int32_t human : {0, 1}) {
                GalacticEvent buy;
                buy.kind = GalacticEventKind::BlackMarketPurchase;
                buy.player = pEmpire;
                buy.planet = story_test::rA;
                buy.value = 0;
                buy.other_player = human;
                gs->post_event(buy);
            }
        }
        if (t == 12) {
            out.scoped_ok = story.log_text().find(" Gui ") == std::string::npos;  // not yet: only Underworld clicked
            story.notify_ui({StoryUiEvent::ClickGui, pEmpire, 0, "Button_X"});
            // P4 lua: nothing yet; then Underworld's click-time BUY_BLACK_MARKET and Underworld's sabotage, both of which
            // reach the Empire's plot set (no player scoping).
            out.special_ok = story.log_text().find(" Bought ") == std::string::npos &&
                             story.log_text().find(" Sabotage ") == std::string::npos;
            story.notify_ui({StoryUiEvent::BuyBlackMarket, pUnder, 0, "Disruptor_Tech"});
            GalacticEvent sab;
            sab.kind = GalacticEventKind::GalacticSabotage;
            sab.player = pUnder;
            sab.planet = story_test::rA;
            sab.type = story_test::tSB1;
            sab.value = kSabotageSpecialStructure;
            gs->post_event(sab);
        }
        if (t == 400) gs->set_destroyed(story_test::rB, true);
        // S10 battle results: the Empire wins at A (counted, planet not listed), Underworld loses (not the
        // Empire's plot), the Empire wins at C (2nd win, listed: fires), then loses and retreats at C.
        auto battle = [gs](GalacticEventKind k, int32_t player, int32_t other, PlanetIndex planet) {
            GalacticEvent e;
            e.kind = k;
            e.player = player;
            e.other_player = other;
            e.planet = planet;
            e.value = 2;
            gs->post_event(e);
        };
        if (t == 450) {
            battle(GalacticEventKind::BattleWon, pEmpire, pUnder, story_test::rA);
            battle(GalacticEventKind::BattleLost, pUnder, pEmpire, story_test::rA);
        }
        if (t == 455) out.battles_ok = story.log_text().find(" Won_Two_At_C ") == std::string::npos &&
                                       story.log_text().find(" Lost_One ") == std::string::npos;
        if (t == 460) battle(GalacticEventKind::BattleWon, pEmpire, pUnder, story_test::rC);
        if (t == 470) {
            battle(GalacticEventKind::BattleLost, pEmpire, pUnder, story_test::rC);
            battle(GalacticEventKind::BattleRetreat, pEmpire, -1, story_test::rC);
        }
        host.tick_once();
    }
    CHECK(landed && killed);
    out.log = story.log_text("S4 wave 1 story notifications");
    out.lua = story.lua_messages();
    const auto& d = story.model(0).dialog_log();
    out.dialogs = d.size();
    if (!d.empty()) {
        out.dialog_tag = d[0].tag;
        out.dialog_chapter = d[0].chapter;
    }
    // q6 t14: the holocron record of that fire, saved with the session (v3) and re-linked to its event on load.
    if (const lua::StoryHolocron* h = story.holocron()) {
        out.holocron = h->size();
        story.mark_holocron_read(0);
        ByteWriter w;
        story.write(w);
        ByteReader rd(w.bytes());
        out.holocron_saved = h->size() == 1 && story.read(rd) && h->size() == 1 && h->entries()[0].tag == "Era_Tag" &&
                             h->entries()[0].read && h->entries()[0].model == &story.model(0) && h->entries()[0].chapter == 2;
    }
    return out;
}
}  // namespace story_wave1

static void test_story_wave1() {
    using namespace story_wave1;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_wave1";
    std::error_code ec;
    remove_all_long(root, ec);
    const auto types = make_types(root / "types");
    write_file(root / "Data/XML/Story_Plots_Wave1.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Wave1</Lua_Script><Active_Plot>Story_Wave1.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Wave1.xml", kPlot);
    write_file(root / "Data/Scripts/Story/Story_Wave1.lua", kScript);
    DirectoryProvider files(path_to_utf8(root));
    const Result a = run(types->types, files, 0);
    Result b = a;
    for (int32_t workers : jobs::determinism_worker_counts()) {  // 1 / 4 / 15: logs must not change
        if (workers == 0) continue;
        const Result r = run(types->types, files, workers);
        if (r.log != a.log) b = r;
    }
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    if (g_story_only) std::printf("%s", a.log.c_str());
    CHECK(a.lua.empty());
    for (const char* ev : {"Deploy_C", "Hero_Lost", "Select_Infantry", "Select_A", "Zoom_B", "Zoom_Out", "Gui", "Speech", "Movie",
                           "Generic", "Lost", "Bought", "Sabotage", "Open_C", "Destroyed_B", "Era", "Owner_Done",
                           "Won_Two_At_C", "Lost_One", "Retreat_C"})
        if (a.log.find(std::string(" ") + ev + " ") == std::string::npos) {
            std::printf("FAIL wave1 story event %s did not fire\n", ev);
            ++g_fail;
        }
    CHECK(a.scoped_ok && a.battles_ok && a.dialogs == 1 && a.dialog_tag == "Era_Tag" && a.dialog_chapter == 2);
    CHECK(a.special_ok);  // P4 lua
    CHECK(a.holocron == 1 && a.holocron_saved);  // q6 t14
    CHECK(a.log.find("UiSelectUnit") != std::string::npos && a.log.find("PlanetDestroyed") != std::string::npos);
    CHECK(!a.log.empty() && a.log == b.log);
    std::printf("story wave 1: %zu log bytes, identical for 0/1/4/15 workers: %s\n", a.log.size(), a.log == b.log ? "yes" : "NO");
    remove_all_long(root, ec);
}

// L1/S4 wave 2a: a battle through StorySession::begin_battle / notify_tactical / end_battle on a pushed space mode
// (no TacticalBattle: the test plays the battle host) - STORY_SPACE_TACTICAL loads the tactical plots named by
// its Param1, the tactical plot set + Lua script run on the tactical store (UNIT_PROXIMITY, CHECK_DESTROYED,
// ATTACK_HARDPOINT, COMMAND_UNIT, TACTICAL_DESTROY, DEFEAT_HERO, ELAPSED, AI_NOTIFICATION from the tactical
// script), VICTORY at the end; then the S10 results with location / composition (WIN_BATTLES SPACE +
// FRIENDLY_CONTAINS CLASS_Corvette) and a land loss of the invader (INVASION_BOUNCED). No golden: every event
// must fire once (the negative ones never), identical logs for 0/1/4/15 workers.
namespace story_wave2a {
using namespace alamo::galactic;
using story_test::pEmpire;
using story_test::pUnder;
enum : uint32_t { tHeroShip = story_test::tCount, tZone, tCount2 };

static const char* kGalactic =
    "<Story>\n"
    " <Event Name=\"Go_Space\"><Event_Type>STORY_SPACE_TACTICAL</Event_Type>"
    "<Event_Param1>Story_Plots_Wave2a_Tac.xml</Event_Param1><Event_Param2>PlanetC</Event_Param2></Event>\n"
    " <Event Name=\"Go_Land_Never\"><Event_Type>STORY_LAND_TACTICAL</Event_Type><Event_Param1>X.xml</Event_Param1></Event>\n"
    " <Event Name=\"Won_Space\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>1</Event_Param1>"
    "<Event_Param2>SPACE</Event_Param2><Event_Param3>FRIENDLY_CONTAINS CLASS_Corvette</Event_Param3></Event>\n"
    " <Event Name=\"Won_Land_Never\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>1</Event_Param1>"
    "<Event_Param2>Land</Event_Param2></Event>\n"
    " <Event Name=\"Won_Fighters_Never\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>1</Event_Param1>"
    "<Event_Param3>FRIENDLY_CONTAINS CLASS_Fighter</Event_Param3></Event>\n"
    " <Event Name=\"Bounced_D\"><Event_Type>STORY_INVASION_BOUNCED</Event_Type><Event_Param1>PlanetD</Event_Param1></Event>\n"
    // wave 2b: a pending corruption mission ends with the battle (SCRIPT_GAPS.md §4.7)
    " <Event Name=\"Corrupt_C\"><Event_Type>STORY_CORRUPTION_TACTICAL_COMPLETE</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param2>KIDNAPPING</Event_Param2></Event>\n"
    " <Event Name=\"Corrupt_Failed_Never\"><Event_Type>STORY_CORRUPTION_TACTICAL_FAILED</Event_Type><Event_Param1>PlanetC</Event_Param1></Event>\n"
    // L1/S4 wave 1 tactical UI: a battle-screen speech end also reaches the galactic sets; a tactical selection / HUD
    // click never does (also not after the battle)
    " <Event Name=\"Gal_Speech\"><Event_Type>STORY_SPEECH_DONE</Event_Type><Event_Param1>TAC_SPEECH_01</Event_Param1></Event>\n"
    " <Event Name=\"Gal_Click_Never\"><Event_Type>STORY_CLICK_GUI</Event_Type><Event_Param1>b_reinforcement</Event_Param1></Event>\n"
    " <Event Name=\"Gal_Select_Never\"><Event_Type>STORY_SELECT_UNIT</Event_Type></Event>\n"
    "</Story>\n";
static const char* kTactical =
    "<Story>\n"
    " <Event Name=\"Victory_Empire\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>EMPIRE</Event_Param1></Event>\n"
    " <Event Name=\"Victory_Under_Never\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Underworld</Event_Param1></Event>\n"
    " <Event Name=\"Enemy_Gone\"><Event_Type>STORY_CHECK_DESTROYED</Event_Type><Event_Param1>Underworld</Event_Param1>"
    "<Event_Param2>DESTROY_ALL</Event_Param2></Event>\n"
    " <Event Name=\"Self_Gone_Never\"><Event_Type>STORY_CHECK_DESTROYED</Event_Type><Event_Param1>Empire</Event_Param1>"
    "<Event_Param2>DESTROY_ALL_UNITS</Event_Param2></Event>\n"
    " <Event Name=\"Near_Zone\"><Event_Type>STORY_UNIT_PROXIMITY</Event_Type><Event_Param2>Trigger_Zone</Event_Param2>"
    "<Event_Param3>100</Event_Param3></Event>\n"
    " <Event Name=\"Far_Zone_Never\"><Event_Type>STORY_UNIT_PROXIMITY</Event_Type><Event_Param1>Hero_Ship</Event_Param1>"
    "<Event_Param2>Trigger_Zone</Event_Param2><Event_Param3>100</Event_Param3></Event>\n"
    " <Event Name=\"Gun_Down\"><Event_Type>STORY_ATTACK_HARDPOINT</Event_Type><Event_Param1>HP_Gun</Event_Param1></Event>\n"
    " <Event Name=\"Moved\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>Corvette</Event_Param1>"
    "<Event_Param2>Trigger_Zone</Event_Param2><Event_Param3>500</Event_Param3></Event>\n"
    " <Event Name=\"Corvette_Killed\"><Event_Type>STORY_TACTICAL_DESTROY</Event_Type><Event_Param1>Corvette</Event_Param1>"
    "<Event_Param3>1</Event_Param3></Event>\n"
    " <Event Name=\"Hero_Dies\"><Event_Type>STORY_DEFEAT_HERO</Event_Type><Event_Param1>Hero_Ship</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Tac_Elapsed\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1></Event>\n"
    " <Event Name=\"Script_Saw\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>TAC_SCRIPT</Event_Param2></Event>\n"
    // wave 2b: GUARD_UNIT pairs Param1[i] with Param2[i]; an empty Param2 never fires; DIFFICULTY_LEVEL is never dispatched
    " <Event Name=\"Guarded\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>Hero_Ship Corvette</Event_Param1>"
    "<Event_Param2>Corvette Trigger_Zone</Event_Param2></Event>\n"
    " <Event Name=\"Guard_Never\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>Corvette</Event_Param1></Event>\n"
    " <Event Name=\"Difficulty_Never\"><Event_Type>STORY_DIFFICULTY_LEVEL</Event_Type><Event_Param1>Normal</Event_Param1></Event>\n"
    // wave 2b fog hooks (StoryBattleStart::fog_*_visible, the fog agent wires them)
    // fog follow-up: ENABLE_FOW rewards reach StorySession::set_enable_fow_hook (no Param1 / Param1 0)
    " <Event Name=\"Zone_Seen\"><Event_Type>STORY_FOG_OBJECT_REVEAL</Event_Type><Event_Param1>Trigger_Zone</Event_Param1>"
    "<Reward_Type>ENABLE_FOW</Reward_Type></Event>\n"
    " <Event Name=\"Point_Seen\"><Event_Type>STORY_FOG_POSITION_REVEAL</Event_Type><Event_Param1>60, 0, 0</Event_Param1>"
    "<Reward_Type>ENABLE_FOW</Reward_Type><Reward_Param1>0</Reward_Param1></Event>\n"
    " <Event Name=\"Far_Point_Never\"><Event_Type>STORY_FOG_POSITION_REVEAL</Event_Type><Event_Param1>9000 0 0</Event_Param1></Event>\n"
    // L1/S4 wave 1 tactical UI (notify_ui with `tactical`): SELECT_UNIT by battle-store id, CLICK_GUI (player-scoped),
    // SPEECH_DONE
    " <Event Name=\"Tac_Select\"><Event_Type>STORY_SELECT_UNIT</Event_Type><Event_Param1>Corvette</Event_Param1></Event>\n"
    " <Event Name=\"Tac_Select_Hero_Never\"><Event_Type>STORY_SELECT_UNIT</Event_Type><Event_Param1>Hero_Ship</Event_Param1></Event>\n"
    " <Event Name=\"Tac_Click\"><Event_Type>STORY_CLICK_GUI</Event_Type><Event_Param1>b_reinforcement</Event_Param1></Event>\n"
    " <Event Name=\"Tac_Click_Never\"><Event_Type>STORY_CLICK_GUI</Event_Type><Event_Param1>b_special_weapon</Event_Param1></Event>\n"
    " <Event Name=\"Tac_Speech\"><Event_Type>STORY_SPEECH_DONE</Event_Type><Event_Param1>TAC_SPEECH_01</Event_Param1></Event>\n"
    "</Story>\n";
// The tactical script sees the tactical store (the Empire corvette, its hull) and reports once.
static const char* kTacScript =
    "function Base_Definitions() ServiceRate = 0.1 done = false end\n"
    "function main() while true do\n"
    "  if not done then local c = Find_First_Object('Corvette')\n"
    "    if c and c.Get_Hull() >= 0 and c.Get_Parent_Mode_Object_ID() == nil then Story_Event('TAC_SCRIPT') done = true end end\n"
    "  coroutine.yield(true) end end\n";

struct Types {
    std::unique_ptr<DirectoryProvider> files;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
};
static std::unique_ptr<Types> make_types(const fs::path& root) {
    write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>Wave2a.xml</File></GameObjectFiles>\n");
    write_file(root / "Data/XML/Wave2a.xml",
               "<W>\n"
               " <Planet Name=\"PlanetA\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetB\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetC\"><Behavior>PLANET</Behavior></Planet>\n"
               " <Planet Name=\"PlanetD\"><Behavior>PLANET</Behavior></Planet>\n"
               " <StarBase Name=\"SB1\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>500</Tactical_Health><CategoryMask>Structure</CategoryMask></StarBase>\n"
               // wave 2b: SELECTABLE (CHECK_DESTROYED counts live SELECTABLE objects, SCRIPT_GAPS.md §4.8)
               " <SpaceUnit Name=\"Corvette\"><Victory_Relevant>Yes</Victory_Relevant><Behavior>SELECTABLE</Behavior><Tactical_Health>100</Tactical_Health>"
               "<CategoryMask>Corvette</CategoryMask></SpaceUnit>\n"
               " <GroundInfantry Name=\"Infantry\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>10</Tactical_Health><CategoryMask>Infantry</CategoryMask></GroundInfantry>\n"
               " <GroundStructure Name=\"Barracks\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>200</Tactical_Health><CategoryMask>Structure</CategoryMask></GroundStructure>\n"
               " <SpaceUnit Name=\"Fighters\"><Victory_Relevant>Yes</Victory_Relevant><CategoryMask>Fighter</CategoryMask></SpaceUnit>\n"
               " <SpaceUnit Name=\"Hero_Ship\"><Victory_Relevant>Yes</Victory_Relevant><Behavior>SELECTABLE</Behavior><Is_Named_Hero>Yes</Is_Named_Hero><Tactical_Health>300</Tactical_Health>"
               "<CategoryMask>Frigate</CategoryMask></SpaceUnit>\n"
               " <Marker Name=\"Trigger_Zone\"><Behavior>MARKER</Behavior></Marker>\n"
               "</W>\n");
    auto t = std::make_unique<Types>();
    t->files = std::make_unique<DirectoryProvider>(path_to_utf8(root));
    CHECK(t->db.load(*t->files));
    CHECK(t->types.build(t->db));
    CHECK(t->db.find("Hero_Ship") == tHeroShip && t->db.find("Trigger_Zone") == tZone && t->db.find("Corvette") == story_test::tCorvette);
    return t;
}

static GalacticRules rules() {
    GalacticRules r = story_test::rules();
    r.types.resize(tCount2);
    r.types[tHeroShip].name = "Hero_Ship";
    r.types[tHeroShip].kind = UnitKind::SpaceUnit;
    r.types[tZone].name = "Trigger_Zone";
    r.types[tZone].kind = UnitKind::SpaceUnit;
    return r;
}

struct Result {
    std::string log;
    std::vector<std::string> lua;
    bool loaded = false, attached_during = false, detached_after = false;
    std::vector<std::pair<bool, int32_t>> fow_rewards;  // fog follow-up: ENABLE_FOW (has Param1, atoi(Param1))
};

static objects::ObjectHandle spawn(objects::ObjectStore& st, uint32_t type, int32_t owner, Vec3 p) {
    objects::SpawnParams sp;
    sp.type = static_cast<objects::TypeIndex>(type);
    sp.owner = owner;
    sp.position = p;
    sp.facing = {1.0f, 0.0f, 0.0f};
    return st.create(sp);
}

static Result run(const objects::GameObjectTypes& types, const FileProvider& files, int32_t workers) {
    const GalacticRules r = rules();
    GalacticSetup setup = story_test::setup();
    setup.forces.push_back({pEmpire, story_test::rA, story_test::tCorvette});
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 13;
    cfg.story_crc = true;
    SimHost host(types, js, cfg);
    GalacticSim* gs = host.start_galactic(r, setup);
    CHECK(gs != nullptr);
    if (!gs) return {};
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{pEmpire, "Story_Plots_Wave2a.xml"}};
    CHECK(host.start_story(st));
    if (!host.story()) return {};
    StorySession& story = *host.story();
    for (int i = 0; i < 5; ++i) host.tick_once();
    // The conflict at C: the galactic corvettes of both sides.
    BattleRequest req;
    req.info.kind = ConflictKind::Space;
    req.info.planet = story_test::rC;
    req.info.attacker = pEmpire;
    req.info.defender = pUnder;
    BattleForces def, att;
    def.player = pUnder;
    att.player = pEmpire;
    for (const ObjectHandle h : gs->store().all_objects())
        if (gs->store().alive(h) && gs->store().type(h) == story_test::tCorvette)
            (gs->store().owner(h) == pEmpire ? att : def).start.push_back(h);
    CHECK(att.start.size() == 1 && def.start.size() == 1);
    req.forces = {def, att};
    // The battle host: push the space mode, spawn the tactical objects, attach the tactical story.
    GameMode& mode = host.start_game(ModeKind::Space);
    objects::ObjectStore& ts = mode.primary();
    const ObjectHandle mine = spawn(ts, story_test::tCorvette, pEmpire, {0, 0, 0});
    const ObjectHandle theirs = spawn(ts, story_test::tCorvette, pUnder, {500, 0, 0});
    const ObjectHandle hero = spawn(ts, tHeroShip, pUnder, {500, 60, 0});
    const ObjectHandle zone = spawn(ts, tZone, -1, {60, 0, 0});
    StoryBattleStart bs;
    bs.request = &req;
    bs.store = &ts;
    bs.fog_point_visible = [](int32_t player, double x, double, double) { return player == pEmpire && x < 1000.0; };
    bs.fog_object_visible = [&ts, zone](int32_t, int32_t id) { return id == ts.id(zone); };
    std::vector<std::string> diag;
    Result out;
    story.set_enable_fow_hook([&out](bool has, int32_t v) { out.fow_rewards.emplace_back(has, v); });  // fog follow-up
    out.loaded = story.begin_battle(bs, host.frame(), &diag);
    for (const auto& d : diag) std::printf("  wave2a diag: %s\n", d.c_str());
    for (uint32_t t = 0; t < 60; ++t) {
        if (t == 2) story.notify_tactical({TacticalStoryEvent::HardpointDestroyed, ts.id(theirs), ts.id(mine), -1, "HP_Gun", -1.0});
        if (t == 3) story.notify_tactical({TacticalStoryEvent::UnitCommanded, ts.id(mine), ts.id(zone), -1, "", 300.0});
        if (t == 4) story.notify_tactical({TacticalStoryEvent::UnitGuarding, ts.id(mine), ts.id(zone), -1, "", -1.0});  // wave 2b
        if (t == 5) {
            story.notify_tactical({TacticalStoryEvent::Destroyed, ts.id(hero), 0, -1, "", -1.0});
            ts.destroy(hero);
        }
        if (t == 7) {
            story.notify_tactical({TacticalStoryEvent::Destroyed, ts.id(theirs), ts.id(mine), -1, "", -1.0});
            ts.destroy(theirs);
        }
        if (t == 8) {  // L1/S4 wave 1 tactical UI (the game host's recorded command, applied by notify_ui)
            story.notify_ui({StoryUiEvent::SelectUnit, pEmpire, static_cast<int32_t>(ts.id(mine)), "", true});
            story.notify_ui({StoryUiEvent::ClickGui, pEmpire, 0, "b_reinforcement", true});
            story.notify_ui({StoryUiEvent::ClickGui, pUnder, 0, "b_special_weapon", true});  // not the tactical plot owner
            story.notify_ui({StoryUiEvent::SpeechDone, pEmpire, 0, "TAC_SPEECH_01", true});
        }
        host.tick_once();
    }
    out.attached_during = story.battle_attached() && story.tactical_model() != nullptr;
    story.set_corruption_mission(pEmpire, 2);  // wave 2b: KIDNAPPING mission at C, won by the Empire
    story.end_battle(pEmpire, host.frame());
    out.detached_after = !story.battle_attached();
    story.notify_ui({StoryUiEvent::SelectUnit, pEmpire, static_cast<int32_t>(ts.id(mine)), "", true});  // no battle: dropped
    story.notify_ui({StoryUiEvent::ClickGui, pEmpire, 0, "b_reinforcement", true});
    CHECK(host.end_game_mode());
    // S10 results: the Empire won the space battle at C (snapshot: its corvette), then lost a land battle at D,
    // which Underworld owns (an invasion that bounced; no snapshot of that battle).
    auto battle = [gs](GalacticEventKind k, int32_t player, int32_t other, PlanetIndex planet, int32_t value) {
        GalacticEvent e;
        e.kind = k;
        e.player = player;
        e.other_player = other;
        e.planet = planet;
        e.value = value;
        gs->post_event(e);
    };
    battle(GalacticEventKind::BattleWon, pEmpire, pUnder, story_test::rC, 2);
    host.tick_once();
    battle(GalacticEventKind::BattleLost, pEmpire, pUnder, story_test::rD, 1);
    host.tick_once();
    out.log = story.log_text("S4 wave 2a battles + tactical story");
    out.lua = story.lua_messages();
    return out;
}
}  // namespace story_wave2a

static void test_story_wave2a() {
    using namespace story_wave2a;
    const fs::path root = fs::temp_directory_path() / "alamo_sim_story_wave2a";
    std::error_code ec;
    remove_all_long(root, ec);
    const auto types = make_types(root / "types");
    write_file(root / "Data/XML/Story_Plots_Wave2a.xml", "<Story_Mode_Plots><Active_Plot>Story_Wave2a.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Wave2a.xml", kGalactic);
    write_file(root / "Data/XML/Story_Plots_Wave2a_Tac.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Wave2a_Tac</Lua_Script><Active_Plot>Story_Wave2a_Tac.xml</Active_Plot>"
               "</Story_Mode_Plots>\n");
    write_file(root / "Data/XML/Story_Wave2a_Tac.xml", kTactical);
    write_file(root / "Data/Scripts/Story/Story_Wave2a_Tac.lua", kTacScript);
    DirectoryProvider files(path_to_utf8(root));
    const Result a = run(types->types, files, 0);
    Result b = a;
    for (int32_t workers : jobs::determinism_worker_counts()) {
        if (workers == 0) continue;
        const Result r = run(types->types, files, workers);
        if (r.log != a.log) b = r;
    }
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    if (g_story_only) std::printf("%s", a.log.c_str());
    CHECK(a.lua.empty() && a.loaded && a.attached_during && a.detached_after);
    // fog follow-up: one ENABLE_FOW without Reward_Param1 (Zone_Seen) and one with 0 (Point_Seen).
    CHECK(a.fow_rewards.size() == 2 && std::count(a.fow_rewards.begin(), a.fow_rewards.end(), std::make_pair(false, int32_t{0})) == 1 &&
          std::count(a.fow_rewards.begin(), a.fow_rewards.end(), std::make_pair(true, int32_t{0})) == 1);
    for (const char* ev : {"Go_Space", "Won_Space", "Bounced_D", "Victory_Empire", "Enemy_Gone", "Near_Zone", "Gun_Down", "Moved",
                           "Corvette_Killed", "Hero_Dies", "Tac_Elapsed", "Script_Saw", "Corrupt_C", "Guarded", "Zone_Seen", "Point_Seen",
                           "Tac_Select", "Tac_Click", "Tac_Speech", "Gal_Speech"})
        if (a.log.find(std::string(" ") + ev + " ") == std::string::npos) {
            std::printf("FAIL wave2a story event %s did not fire\n", ev);
            ++g_fail;
        }
    // L1/S4 wave 1 tactical UI: fired by the notifications (not by SPEECH_DONE's 60 s timeout).
    for (const char* want : {" Tac_Select STORY_SELECT_UNIT", " Tac_Click STORY_CLICK_GUI", " Tac_Speech STORY_SPEECH_DONE",
                             " Gal_Speech STORY_SPEECH_DONE"}) {
        const size_t at = a.log.find(want);
        const std::string line = at == std::string::npos ? std::string() : a.log.substr(at, a.log.find('\n', at) - at);
        const char* via = std::string_view(want).substr(0, 5) == " Gal_" ? "via=trigger:UiSpeechDone#" : "via=trigger:Tac";
        if (line.find(via) == std::string::npos) {
            std::printf("FAIL wave2a tactical UI:%s not fired by its notification (%s)\n", want, line.c_str());
            ++g_fail;
        }
    }
    for (const char* ev : {"Go_Land_Never", "Won_Land_Never", "Won_Fighters_Never", "Victory_Under_Never", "Self_Gone_Never",
                           "Far_Zone_Never", "Corrupt_Failed_Never", "Guard_Never", "Difficulty_Never",
                           "Far_Point_Never", "Tac_Select_Hero_Never", "Tac_Click_Never", "Gal_Click_Never",
                           "Gal_Select_Never"})
        if (a.log.find(std::string(" ") + ev + " ") != std::string::npos) {
            std::printf("FAIL wave2a story event %s fired\n", ev);
            ++g_fail;
        }
    CHECK(a.log.find("BattleStarted") != std::string::npos && a.log.find("BattleEnded") != std::string::npos &&
          a.log.find("# tactical plot set 1:") != std::string::npos);
    CHECK(!a.log.empty() && a.log == b.log);
    std::printf("story wave 2a: %zu log bytes, identical for 0/1/4/15 workers: %s\n", a.log.size(), a.log == b.log ? "yes" : "NO");
    remove_all_long(root, ec);
}

// Real FoC campaign opening: Full_Story_Campaign_Underworld (Story_Plots_Campaign_Underworld.XML +
// Story_Campaign_Underworld.lua), 900 ticks hands-off.
static void test_story_real(const FileProvider& files, const objects::GameObjectTypes& types) {
    data::GameDatabase gdb;
    galactic::GalacticRules rules;
    std::vector<std::string> diag;
    CHECK(gdb.load(files, {}, &diag) && galactic::load_galactic_rules(files, gdb, types, rules, &diag));
    const char* name = "Full_Story_Campaign_Underworld";
    const data::CampaignDef* camp = nullptr;
    for (const auto& c : gdb.campaigns())
        if (c.name == name) camp = &c;
    CHECK(camp != nullptr);
    if (!camp) return;
    const galactic::GalacticSetup setup = galactic::build_campaign_setup(gdb, rules, name, {}, &diag);
    auto run = [&](int32_t workers, std::string& log, CrcLog& crc) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 99;
        cfg.story_crc = true;
        SimHost host(types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(rules, setup);
        CHECK(gs != nullptr);
        if (!gs) return;
        StoryStart st;
        st.files = &files;
        st.plot_sets = campaign_plot_sets(*camp, *gs);
        CHECK(st.plot_sets.size() == 1);
        std::vector<std::string> sd;
        CHECK(host.start_story(st, &sd));
        if (!host.story()) return;
        host.run_fast(900);
        log = host.story()->log_text(std::string(name) + ", seed 99, 900 ticks");
        crc = host.crc_log();
        std::printf("  %s workers %d: %zu story events fired, %zu triggers, %zu Lua messages, final CRC %016" PRIx64 "\n", name,
                    workers, host.story()->log().size(), host.story()->triggers().size(), host.story()->lua_messages().size(),
                    crc.records.back().combined);
        if (workers == 0)
            for (size_t i = 0; i < host.story()->lua_messages().size() && i < 5; ++i)
                std::printf("    lua: %s\n", host.story()->lua_messages()[i].c_str());
    };
    std::string la, lb;
    CrcLog ca, cb;
    run(0, la, ca);
    run(4, lb, cb);
    CHECK(!la.empty() && la == lb && !first_divergence(ca, cb).diverged);
    check_text_golden("story_underworld_golden.txt", la);
}
#endif

// -------------------------------------------------------------------------------------- real data
static void test_real_data() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    objects::ObjectTypeDatabase db;
    CHECK(db.load(*files));
    objects::GameObjectTypes types;
    CHECK(types.build(db, files.get()));
    const TypeCrcIndex index(types);
    // S3: a FoC galactic campaign with the real economy as the galactic mode tick, then a space battle
    // pushed over it (the galaxy freezes), popped again.
    {
        data::GameDatabase gdb;
        galactic::GalacticRules rules;
        std::vector<std::string> diag;
        CHECK(gdb.load(*files, {}, &diag) && galactic::load_galactic_rules(*files, gdb, types, rules, &diag));
        const galactic::GalacticSetup setup =
            galactic::build_campaign_setup(gdb, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
        auto run = [&](int32_t workers, bool battle) {
            jobs::JobSystem js(jobs::JobSystemConfig{workers});
            SimConfig cfg;
            cfg.seed = 99;
            SimHost host(types, js, cfg);
            galactic::GalacticSim* gs = host.start_galactic(rules, setup);
            CHECK(gs != nullptr);
            if (!gs) return CrcLog{};
            host.run_fast(1500);
            CHECK(gs->frame() == 1500 && gs->galactic_day() >= 1);
            host.galactic_mode()->push_pause();
            host.run_fast(30);
            CHECK(gs->frame() == 1500 && host.galactic_mode()->frame() == 1500);
            host.galactic_mode()->pop_pause();
            if (battle) {
                host.start_game(ModeKind::Space);
                host.run_fast(60);
                CHECK(gs->frame() == 1500 && host.frame() == 1590);
                host.end_game_mode();
            }
            host.run_fast(1200);
            CHECK(gs->frame() == 2700 && host.galactic_mode()->frame() == 2700);
            std::printf("  galactic HotM workers %d: frame %d, cycles %d, objects %zu, final CRC %016" PRIx64 "\n", workers,
                        gs->frame(), gs->galactic_day(), gs->store().live_count(), host.crc_log().records.back().combined);
            return host.crc_log();
        };
        const CrcLog a = run(0, true);
        const CrcLog b = run(4, true);
        CHECK(!first_divergence(a, b).diverged && a.records.size() == 2791);
        const std::string golden = testdata("galactic_hotm_golden.crc");
        std::string gerr;
        if (g_regen) {
            CrcLog g = a;
            g.comments = {"G0 + S3: Sandbox_Heart_Of_The_Maw_Underworld, seed 99, 1500 + 30 paused + 60 space + 1200 ticks"};
            CHECK(g.save(golden, &gerr));
        } else {
            CrcLog g;
            CHECK(CrcLog::load(golden, g, &gerr));
            const auto d = first_divergence(g, a);
            std::printf("  golden galactic_hotm_golden.crc: %s\n", d.message.c_str());
            CHECK(!d.diverged);
        }
    }
#ifdef ALAMO_SIM_HAVE_STORY
    test_story_real(*files, types);
#endif
    for (const char* name : {"_SPACE_PLANET_CORELLIA_PIRACY", "_LAND_PLANET_BYSS_KIDNAPPING"}) {
        terrain::MapData map;
        const bool map_loaded = terrain::load_map(*files, name, map, &err);
        if (!map_loaded && files->mounts().empty()) {  // maps live in maps.meg: text-only data (unzipped extracted/*.zip) has none
            std::printf("%s: not in this data set (loose text data, no MEGs mounted): map checks skipped\n", name);
            continue;
        }
        CHECK(map_loaded);
        // Spawn + spatial query sanity on the real map.
        jobs::JobSystem js(jobs::JobSystemConfig{4});
        SimHost host(types, js);
        host.start_game(map.is_space() ? ModeKind::Space : ModeKind::Land);
        TacticalMode& tm = *host.tactical();
        const SpawnReport r = spawn_map(tm, map, index, host.parallel_for());
        CHECK(r.unknown_type == 0 && r.spawned == map.objects.size() && r.units > 10 && r.in_trees == r.spawned);
        const objects::ObjectStore& st = tm.primary();
        uint32_t health_ok = 0;
        for (size_t i = 0; i < map.objects.size(); ++i) {
            const ObjectHandle h = st.find(static_cast<ObjectId>(i + 1));
            const float expect = map.objects[i].health_fraction * types.columns().tactical_health[st.type(h)];
            if (st.health(h) == expect) ++health_ok;
        }
        CHECK(health_ok == map.objects.size());
        tactical::QueryContext ctx;
        uint32_t checked = 0;
        for (size_t i = 0; i < st.all_objects().size(); i += 7) {
            const ObjectHandle c = st.all_objects()[i];
            const Vec3 p = st.position(c);
            tactical::CircleXY circle;
            circle.center = {p.x, p.y, p.z};
            circle.radius = 1500.0f;
            std::vector<tactical::EntityHandle> brute;
            for (ObjectHandle h : st.all_objects()) {
                const auto* m = tm.trees().membership(static_cast<uint32_t>(st.id(h)));
                if (m && m->projectile_collidable && tactical::overlaps(circle, tm.bounds_of(h)))
                    brute.push_back(static_cast<uint32_t>(st.id(h)));
            }
            std::sort(brute.begin(), brute.end());
            const auto got = tm.trees().all().query_circle_xy(circle, tactical::kAllCategories, ctx);
            if (std::vector<tactical::EntityHandle>(got.begin(), got.end()) == brute) ++checked;
            else CHECK(false);
        }
        std::printf("%s: %u placements -> %u objects (%u units, %u structures, %u props), all() tree %u, %u radius queries = brute force\n",
                    name, r.placements, r.spawned, r.units, r.structures, r.props, tm.trees().all().size(), checked);
        determinism(types, map, name, 600);
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--regen") == 0) g_regen = g_regen_story = true;
        else if (std::strcmp(argv[i], "--regen-story") == 0) g_regen_story = true;
        else if (std::strcmp(argv[i], "--story-only") == 0) g_story_only = true;
    test_pacing();
    const auto world = make_world();
#ifdef ALAMO_SIM_HAVE_STORY
    test_story_synthetic(*world);
    test_story_rewards_oi(*world);  // OI lua
    test_story_t9_clock(*world);    // host follow-up: T9 time argument
    test_story_load_pass(*world);   // A2i t01: load-time STORY_TECH_LEVEL pass, ELAPSED 0 at tick 2
    test_story_load_construct(*world);  // a2j t05: starting-force / spawn CONSTRUCT and the load-time event stream
    test_story_wave1();
    test_story_wave2a();
    story_test::test_many_players(*world);  // Q6
    story_test::test_scoring_script(*world);  // q6 t06
    story_test::test_scoring_service(*world);  // q6 t09
    story_test::test_scoring_story(*world);    // q6 t09
    story_test::test_story_handle_esc(*world);  // bkc1 t01
    story_test::test_q6_story_chain(*world);   // q6: load-time resume, Perpetual ELAPSED, SWITCH_CONTROL hook
    object_script_test::test_object_scripts();  // q6: game-object Lua scripts
#else
    std::printf("story: built without engine/lua, skipped\n");
#endif
    if (g_story_only) {
        std::printf(g_fail ? "alamo_sim_tests: %d failure(s)\n" : "alamo_sim_tests: OK (story only)\n", g_fail);
        return g_fail ? 1 : 0;
    }
    test_mode_stack(*world);
    test_tick_order(*world);
    test_commands(*world);
    test_select_all();  // bkc1 t04
    test_realtime_equals_fast(*world);
    test_save_session(*world);  // N1
    determinism(world->types, make_synthetic_map(), "synthetic", 600);
    test_real_data();
    std::printf(g_fail ? "alamo_sim_tests: %d failure(s)\n" : "alamo_sim_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
