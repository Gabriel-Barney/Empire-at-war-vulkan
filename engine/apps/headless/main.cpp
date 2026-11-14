// alamo_headless - Phase 2 headless simulation host + Q1 determinism replay driver.
//
//   alamo_headless --data <dir> [--campaign NAME] [--frames N] [--workers N] [--ais K]
//   alamo_headless --game <corruption dir> [MODPATH=..] [FALLBACKPATH=..] [...]
//   alamo_headless --data <dir> --list            list campaigns
//   alamo_headless --data <dir> --all             run every campaign, summary table
//   alamo_headless --synthetic N [...]            N-planet random galaxy (mod-scale stress)
//   alamo_headless --selfcheck                    synthetic run at 0/1/4/N workers, per-tick CRCs must match
//   --latency F    AI all-pairs recomputes run as background jobs published F frames after the kick (OPT2 t02:
//                  default galactic::kDefaultAllPairsLatency = 15; 0 = the original timing, computed and read on
//                  the same frame)
//
// Q1 replay / CRC (see docs/PHASE4.md Q1, alamo/core/sim_crc.hpp, alamo/commands/recording.hpp):
//   alamo_headless --replay STREAM [--workers N] [--frames N] [--crc-out FILE] [--golden FILE]
//                                  [--check-workers]
//       Runs the synthetic galaxy described by the stream header (seed, ticks, meta planets/ais/
//       latency) applying its commands, records one CRC record per tick (systems galaxy.graph,
//       commands, rng, ai.state, fleet_orders). --golden compares against a stored CRC file and
//       reports the first diverging tick + system; --check-workers runs every worker count in
//       0/1/4/N and requires identical CRC sequences.
//   alamo_headless --record-stream FILE [--seed S] [--frames N] [--synthetic P] [--ais K] [--latency F]
//       Writes a random (seeded, deterministic) command stream for replays/tests.
//   alamo_headless --compare EXPECTED.crc ACTUAL.crc
//       Prints the first diverging tick and system; exit 1 if the logs differ. A checkpoint log (ALAMO_CRC 2) on
//       either side is compared on its stored ticks only; a mismatch names the first bad checkpoint, its window of
//       ticks and the differing columns.
//   --crc-out FILE [--crc-checkpoint N [--crc-keep T,T,...]] (every run mode that writes a CRC log): by default the full
//       per-tick log (ALAMO_CRC 1); with --crc-checkpoint N the checkpoint log (ALAMO_CRC 2: first and last tick, every
//       multiple of N and the --crc-keep ticks). The checked-in goldens are checkpoint logs (docs/status/golden-checkpoints.md);
//       --golden accepts either form.
//   alamo_headless --crc-convert IN.crc OUT.crc [--crc-checkpoint N (default 64)] [--crc-keep T,T,...]
//       Rewrites a CRC log as a checkpoint log (same writer as --crc-checkpoint; tools/goldens/convert_goldens.py is the
//       same rule in Python and a ctest checks they agree byte for byte).
//
// G0 / S5 tactical host (engine/apps/sim; docs/PHASE4.md S5/G0):
//   alamo_headless --map NAME [--game DIR] [--ticks N] [--workers W] [--seed S] [--no-ai] [--realtime]
//                  [--record-orders FILE.acmd | --replay FILE.acmd] [--crc-out FILE] [--golden FILE]
//                  [--check-workers]
//       Spawns the map's objects (types from GameObjectFiles.xml, .ted placements) into a land or
//       space mode and runs N ticks (as fast as possible, or paced at 30 ticks/s with --realtime) with
//       seeded stand-in player orders (scheduled, recorded with --record-orders) and AI stand-ins
//       (execute_direct); --replay applies a recorded stream instead. Prints every tick's CRC and the
//       final CRC. --game defaults to ALAMO_FOC_DATA.
//   alamo_headless --economy (--game DIR | --data DIR) [--campaign NAME] [--ticks N] [--workers W] [--seed S]
//                  [--check-workers] [--crc-out FILE] [--golden FILE]
//       A galactic campaign through the G0 host with the S3 economy (GalacticSim) as the galactic
//       mode tick: credits, income, production, fleets, ownership. Prints credits / planets per player.
//   alamo_headless --story (--game DIR | --data DIR) [--campaign NAME] [--ticks N] [--workers W] [--seed S]
//                  [--check-workers] [--story-out FILE] [--story-golden FILE] [--crc-out FILE] [--golden FILE]
//       S4: --economy plus the campaign's story plots + Lua scripts (engine/apps/sim story_session.hpp).
//   alamo_headless --economy --battles auto|fight|fight-all [--aggressor FRAMES] [--battle-retreat R] ...
//       S10: galactic conflicts queued, decided and auto-resolved or fought as tactical battles
//       (battle_run.hpp); the [port] aggressor stand-in moves fleets toward hostile planets.
//   alamo_headless --economy --galactic-ai [--battles auto|fight|fight-all] [--ai-log FILE] [--ai-golden FILE] ...
//       A2: AI-vs-AI galactic campaign, every playable faction a galactic AI player (ai_run.hpp).
//       [--human FACTION [--human-script FILE]]: that faction is a scripted human (fixed recorded command stream).
//       [--ai-type FACTION=TYPE ...]: that faction's AI player type from the start instead of the campaign's AI_Player_Control
//       (q6 t07 [port] test knob: as a SWITCH_CONTROL story reward at frame 0, e.g. Thrawn's Revenge's INITIALIZE_AI types).
//       [--trace-out FILE]: per-tick Q2TRACE 1 state trace (port_trace.hpp) for tools/probe_mod/q2_traces/q2trace.py; with
//       --skirmish-ai (q4hf t02) the tactical battle's trace (tactical_trace.hpp: owner / type / hull per object, first shot).
//       [--story]: also run the campaign's story plot sets, as the retail sandbox does (q2f t03).
//       [--ai-latency F] [--no-ai-stagger | --ai-stagger] (also --skirmish-ai): OPT2 t02 / t01 parity knobs - the AI path
//       recompute latency (default 15 frames, 0 = the original timing) and every AI player planning on the same frames (the
//       original; the galactic AI default since a2m r4, --ai-stagger restores the OPT2 per-player phases there; the
//       tactical / skirmish AI keeps the phases unless --no-ai-stagger).
//       [--no-pp-streams] [--pp-latency F] (--skirmish-ai): OPT2 t10 parity knobs - slot-10 plan picks and PotentialPlan
//       solves on the synced stream in proposal order (the original), and the frames from a goal slice to the apply of
//       its solves (default 1; 0 = in the slice, on the job system).
//       [--ai-lane] [--ai-lane-k K] (any run with a tactical AI, --bench included): sp3 AI lane (docs/SIM_PARALLEL_DESIGN.md
//       5.1), off by default; K = ticks from an AI update to the apply of its command batch (default 2).
//   alamo_headless --skirmish-ai synthetic|foc|foc-land|foc-land-campaign|foc-station|skirmish [--map NAME] [--ticks N] [--crc-out F] [--golden F] [--check-workers]
//       [--expect-battle-end]: fail unless the battle ends with a winner before its frame limit (S10 land outcome).
//       [--expect-skirmish-base]: foc-land only (t50): fail unless both sides start with a pre-built base, the AI acts and shots are fired.
//       skai (q2f t04): foc-land and skirmish are the skirmish host: Empire (slot 0) vs Rebel (slot 1) on an _MP_ map
//       (skirmish: --map, default _MP_SPACE_GEONOSIS), MP_Default_Credits, the factions' skirmish default forces, the battle
//       started on the map; [--human FACTION]: that slot is an idle human (no AI, no orders).
//       q6 t02: [--ai FACTION[,FACTION]] the slots' factions (mods, MODPATH=): two names = slot 0, slot 1 (--human may name one);
//       one name = the AI slot (slot 1) beside the --human faction (slot 0). Each AI slot plays its faction's Basic_AI, starts with
//       its Space_ / Land_Skirmish_AI_Default_Forces (else the cheapest buildable units, [port]).
//       A1 stage 2: AI-vs-AI space skirmish fought by the tactical AI in an S10 battle (skirmish_run.hpp); foc-land: a land skirmish (t50), foc-land-campaign: the campaign-rules land invasion.
//       [--fail-on-lua-errors]: fail on any tactical AI Lua error (foc-station always does; a1j t07).
//       [--squadron-size N] (any mode, b0319): fighter / bomber squadron members for every type database this run builds;
//       0 = the retail lists and prices (parity harness, retail-outcome tests), default GameObjectTypes::kPortSquadronSize.
//       [--control-points]: foc-land / skirmish on a land map (ccpgt t02): the Capture Control Points game type, each slot's win
//       condition 0x15 CONTROL_POINT_DOMINATION (SKIRMISH_SETUP §4.2) instead of 0x13.
//       Prints and writes the fired-event log (frame, plot set:sequence, plot, event, type, params,
//       reward, cause) with the game-side trigger stream, and per-tick CRCs incl. "story". Campaign
//       default Full_Story_Campaign_Underworld.
// N1 save / load (save_run.hpp, engine/apps/sim save_session.hpp):
//   alamo_headless --save-session synthetic|hotm|galactic|skirmish [--campaign NAME] --save-at T FILE [--ticks K] [--crc-out F]
//       [--battles auto|fight|fight-all[+anim]] [--story] [--map M] [--save-when any|battle|galactic|pending|scheduled]   (stage 2, save_run.hpp)
//       Runs a session to tick T, saves it to FILE, runs K more ticks.
//   alamo_headless --load FILE [--ticks K] [--crc-out F] [--replay STREAM.acmd] [--record-orders OUT.acmd]
//       Rebuilds the saved session from its descriptor, loads it and runs K ticks (replaying / recording orders).
//   alamo_headless --save-session KIND --save-check --save-at T FILE --ticks K
//       N1 done-check: save at T, continue to T+K; load in this process, in a fresh process and with replays, all
//       must give the original's per-tick CRCs.
// SP5: any mode takes --svc-mode serial|parallel|reverse (the battle services graph; same results, reverse = order check)
// and --svc-check (debug write check of the services' declared sets; violations printed to stderr), and
// --svc-streams hangar|none (SP5 t03: those services draw from counter-based streams; this MOVES per-seed outcomes), and
// --svc-parallel-min N (the per-unit service loops fork from N units, default 256; same results, a check of the forks).
// BM1 benchmark (bench_run.hpp; scenario format alamo/sim/bench_scenario.hpp):
//   alamo_headless --bench SCENARIO.abench [--runs R] [--warmup W] [--workers N] [--ticks K] [--bench-json FILE|-]
//                  [--bench-baseline OLD.json] [--crc-out F] [--golden F] [--bench-allow-changed]
//       Sim-only mode: the scenario's save + recorded orders, its ticks as fast as possible; median / p99 / worst tick, OPT1
//       zones, CRCs against the scenario fingerprint (exit 3 "scenario changed, not comparable" on a mismatch).
//   alamo_headless --bench-make SCENARIO.abench --save-session KIND [...] --bench-save-at T --ticks K [--bench-every N]
//   alamo_headless --bench-regen SCENARIO.abench    (remake with the manifest's recorded arguments)
//
// --data takes a loose folder containing Data\XML (e.g. extracted foc_data_text.zip). --game uses
// the real VFS (loose dirs + MEGs + EaW fallback). AI inputs come from S2 perception (engine/ai):
// a campaign's starting-state snapshot (--data/--game) or a synthetic snapshot (synthetic galaxies,
// replays), perturbed every 180 frames until S3 drives it; "ai.perception" is in the CRC log.
// Each AI recomputes its all-pairs matrix every 90 frames (Q11 cadence: first at creation,
// staggered by slot) and
// issues fleet A* requests every frame. The run prints a checksum over everything the AIs read;
// it must be identical for any worker count.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <variant>
#include <span>
#include <string>
#include <vector>

#include "alamo/ai/galactic_perception.hpp"
#include "alamo/ai/galactic_seed.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/commands/recording.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/cadence.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/galactic_rules.hpp"  // cu5 t01: load_factions (campaign seed options)
#include "alamo/galactic/planet_graph.hpp"
#include "alamo/jobs/jobs.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_HEADLESS_HAVE_SIM
#include "alamo/sim/battle_ai.hpp"       // sp3: --ai-lane (ai_lane_defaults)
#include "alamo/sim/bench_scenario.hpp"  // BM1 --bench-regen
#ifdef ALAMO_SIM_HAVE_BATTLES
#include "alamo/sim/tactical_battle.hpp"  // SP5 --svc-mode / --svc-check
#include "alamo/tactical/services/stream_rng.hpp"  // SP5 --svc-streams
#endif
#endif
#include "alamo/vfs/steam_workshop.hpp"
#include "alamo/vfs/vfs.hpp"
#include "crc_out.hpp"  // header-only (core): the --crc-checkpoint options parse without the sim too
#ifdef ALAMO_HEADLESS_HAVE_SIM
#include "ai_run.hpp"  // A2
#include "skirmish_run.hpp"  // A1 stage 2
#include "battle_run.hpp"
#include "bench_run.hpp"  // BM1
#include "alamo/jobs/topology.hpp"  // sp1: OS hints
#include "map_run.hpp"
#include "save_run.hpp"  // N1
#endif

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

constexpr uint32_t kRecomputePeriod = 90;     // GALACTIC.md §4: every 90 frames per AI

struct Options {
    std::string data_dir;
    std::string game_dir;
    std::vector<std::string> game_args;
    std::string campaign;
    uint32_t frames = 900;
    bool frames_set = false;
    int32_t workers = -1;
    uint32_t ais = 2;
    bool ais_set = false;
    uint32_t latency = alamo::galactic::kDefaultAllPairsLatency;  // 0 = vanilla timing; >0 = background, visible k frames later
    uint32_t synthetic = 0;
    bool list = false;
    bool all = false;
    bool selfcheck = false;
    bool quiet = false;
    // Q1
    std::string replay, crc_out, golden, record_stream, compare_a, compare_b;
    std::string convert_in, convert_out;  // --crc-convert IN OUT
    bool check_workers = false;
    uint64_t seed = 1;
    // G0 map host
    std::string map, record_orders;
    uint64_t ticks = 600;
    bool ticks_set = false, realtime = false, no_ai = false, economy = false;
    // S4 story
    bool story = false;
    bool scoring = false;  // q6 t06: --scoring
    bool object_scripts = true;  // q6: --skirmish-ai, off with --no-object-scripts (q6 t23)
    bool control_points = false;  // ccpgt t02: --control-points
    std::string story_out, story_golden;
    // S10 battle transitions (battle_run.hpp): --battles auto|fight|fight-all
    std::string battles;
    uint32_t aggressor = 600;
    float battle_retreat = 0.0f;
    uint32_t battle_max_frames = 27000;
    // A2 galactic AI (ai_run.hpp): --galactic-ai [--ai-log FILE] [--ai-golden FILE]
    bool galactic_ai = false;
    std::string ai_log, ai_golden;
    std::string ai_human, ai_human_script;  // A2 galactic: --human FACTION [--human-script FILE]
    std::vector<std::string> ai_types;      // q6 t07: --ai-type FACTION=TYPE (repeatable)
    std::string trace_out;                  // q2f t05: --trace-out FILE (Q2TRACE 1 state trace, galactic AI runs; q4hf t02: --skirmish-ai)
    int64_t ai_latency = -1;                // OPT2 t02: --ai-latency F (galactic / tactical AI path recomputes)
    int8_t ai_stagger = -1;                 // OPT2 t01: --no-ai-stagger 0, --ai-stagger 1, -1 = the mode's default
    bool pp_streams = true;                 // OPT2 t10: --no-pp-streams
    int64_t pp_latency = -1;                // OPT2 t10: --pp-latency F
    // A1 stage 2 tactical AI (skirmish_run.hpp): --skirmish-ai synthetic|foc
    std::string skirmish_ai;
    std::string skirmish_factions;  // q6 t02: --ai FACTION[,FACTION]
    bool expect_battle_end = false;  // S10 land outcome: --expect-battle-end
    bool expect_skirmish_base = false;  // t50: --expect-skirmish-base
    bool fail_on_lua_errors = false;  // a1j t07: --fail-on-lua-errors
    // N1 save / load (save_run.hpp)
    std::string save_session, save_file, load_file;
    std::string save_when = "any";  // N1 stage 2: --save-when any|battle|galactic|pending|scheduled
    uint64_t save_at = 0;
    bool save_at_set = false, save_check = false;
    bool seed_set = false;
    // BM1 benchmark (bench_run.hpp)
    std::string bench, bench_make, bench_regen, bench_json, bench_baseline, bench_desc;
    uint32_t bench_runs = 3, bench_warmup = 1, bench_every = 1;
    bool bench_allow_changed = false, workers_set = false;
    uint64_t bench_save_at = 0;
    bool bench_save_at_set = false;
    std::vector<int> game_arg_index;  // argv indices parsed as game args (left out of a scenario's recorded make line)
};

// Deterministic stand-in for the synced RNG / perception values (real perception comes later).
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
};

alamo::GalaxyMap make_synthetic(uint32_t n, uint32_t seed) {
    alamo::GalaxyMap m;
    m.campaign_name = "Synthetic_" + std::to_string(n);
    m.max_fleet_move_distance = 100.0f;
    Lcg r(seed);
    const float extent = 60.0f * static_cast<float>(n) / 10.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const auto p = m.planets.add("P" + std::to_string(i));
        m.planets.flags[p] = alamo::kPlanetHasPlanetBehavior;
        m.planets.position[p] = {static_cast<float>(r.next() % 100000u) / 100000.0f * extent,
                                 static_cast<float>(r.next() % 100000u) / 100000.0f * extent, 0.0f};
    }
    for (uint32_t i = 0; i < n && n > 1; ++i) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t j = r.next() % n;
            if (j == i) continue;
            const auto rt = m.routes.add("R" + std::to_string(i) + "_" + std::to_string(j) + "_" + std::to_string(k));
            m.routes.point_a[rt] = i;
            m.routes.point_b[rt] = j;
            m.routes.hs_speed_factor[rt] = (r.next() % 2) ? 5.0f : 2.5f;
        }
    }
    return m;
}

// ---- Q1 replay commands --------------------------------------------------------------------------
// The replay uses engine/commands (G0-3): a CommandRecording (.acmd) replayed through a
// CommandPipeline whose handler applies each command to this host's stand-in galactic state.
// Mapping (header.player = AI slot). These are host test hooks, so they use the port-only
// PortExtension event (id 63; subtype + opaque data), not the original's event types:
//   0x8001 SetPerception: u32 planet | f64 value      (perception override for that AI)
//   0x8002 SetRevealed:   u32 planet | u8 revealed    (fog mask bit)
//   0x8003 FleetMove:     u32 source planet + 1 | u32 destination planet + 1 | u8 A* mode (0..5)
// Anything else, or bad indices/payloads, is rejected by the handler (counted in the "commands"
// CRC system, as is the pipeline's per-tick canonical batch CRC).
namespace cmd = alamo::commands;
constexpr uint16_t kRawSetPerception = 0x8001;
constexpr uint16_t kRawSetRevealed = 0x8002;
constexpr uint16_t kRawFleetMove = 0x8003;

struct RunResult {
    std::string name;
    uint32_t planets = 0, routes = 0, edges = 0;
    uint32_t recomputes = 0, skipped = 0, paths = 0, reachable = 0;
    uint32_t cmd_applied = 0, cmd_rejected = 0;
    double graph_ms = 0, recompute_ms_total = 0, recompute_ms_max = 0, astar_ms_total = 0, run_ms = 0;
    uint64_t checksum = 0;
};

uint64_t mix(uint64_t h, const void* p, size_t n) { return alamo::fnv1a64(p, n, h); }

// `recording` (optional) is replayed through an engine/commands pipeline; `crc_log` (optional) receives one CRC record per
// tick; `rng_seed` seeds the synced game RNG streams.
RunResult run_campaign(const alamo::GalaxyMap& map, const Options& opt, int32_t workers,
                       const alamo::commands::CommandRecording* recording = nullptr, alamo::CrcLog* crc_log = nullptr,
                       uint64_t rng_seed = 0, const alamo::ai::GalacticSnapshot* initial_snapshot = nullptr) {
    namespace gal = alamo::galactic;
    RunResult res;
    res.name = map.campaign_name;
    res.planets = static_cast<uint32_t>(map.planets.size());
    res.routes = static_cast<uint32_t>(map.routes.size());

    alamo::jobs::JobSystem js(alamo::jobs::JobSystemConfig{workers});
    const alamo::ParallelFor pf = alamo::jobs::make_parallel_for(js);

    auto t0 = Clock::now();
    const gal::PlanetGraph graph = gal::PlanetGraph::build(map, {}, pf);
    res.graph_ms = ms_since(t0);
    res.edges = graph.edge_slots() / 2;
    const uint32_t n = graph.size();

    // S2 perception (engine/ai): the galactic snapshot (campaign starting state, or a deterministic
    // synthetic galaxy) evolves through perturb_snapshot every 2 recompute windows (no S3 sim yet),
    // GalacticPerception refreshes every AI's memory each frame and provides the Node_Weight inputs,
    // the all-pairs fog mask and every PlayerView field. Replay commands override on top.
    namespace aip = alamo::ai;
    aip::GalacticSnapshot snap =
        initial_snapshot ? *initial_snapshot : aip::make_synthetic_snapshot(n, opt.ais, static_cast<uint32_t>(rng_seed));
    const auto snap_seed = static_cast<uint32_t>(rng_seed) ^ 0xA15EEDu;
    aip::GalacticPerception perception;
    perception.reset(snap);
    // AI slot a -> the a-th tracked (AI) player of the snapshot.
    std::vector<aip::PlayerId> ai_player;
    for (uint32_t p = 0; p < snap.player_count() && ai_player.size() < opt.ais; ++p)
        if (perception.tracks(static_cast<aip::PlayerId>(p))) ai_player.push_back(static_cast<aip::PlayerId>(p));
    const auto n_ais = static_cast<uint32_t>(ai_player.size());

    struct Ai {
        gal::PlayerView view;
        gal::Bitset revealed;       // all-pairs fog mask (perception + replay overrides)
        bool fog = false;           // fog gate applies (non-human player with an AI system)
        std::vector<double> value;  // perception totals for the current recompute
        std::vector<uint8_t> has_override;   // replay: perception overrides
        std::vector<double> override_value;
        std::vector<int8_t> revealed_override;  // replay: -1 none, else 0 / 1
        gal::AllPairsCache cache;
        std::unique_ptr<alamo::jobs::Deferred<gal::AllPairsMatrices>> deferred;
        int32_t next_recompute = 0;
        uint32_t kicks = 0;
    };
    std::vector<Ai> ais(n_ais);
    for (uint32_t a = 0; a < n_ais; ++a) {
        Ai& ai = ais[a];
        ai.revealed = gal::Bitset(n, true);
        ai.has_override.assign(n, 0);
        ai.override_value.assign(n, 0.0);
        ai.revealed_override.assign(n, -1);
        if (opt.latency > 0) ai.deferred = std::make_unique<alamo::jobs::Deferred<gal::AllPairsMatrices>>(js);
    }
    // Per frame, after perception.update: PlayerView + fog mask from perception, replay overrides on top.
    auto refresh_ai = [&](uint32_t a) {
        Ai& ai = ais[a];
        perception.fill_view(snap, ai_player[a], ai.view);
        const gal::AllPairsInput in = perception.all_pairs_input(ai_player[a]);
        ai.fog = in.revealed != nullptr;
        ai.revealed = (in.revealed && in.revealed->size() == n) ? *in.revealed : gal::Bitset(n, true);
        if (ai.view.revealed.size() != n) ai.view.revealed = gal::Bitset(n, true);
        for (uint32_t p = 0; p < n; ++p) {
            if (ai.revealed_override[p] < 0) continue;
            ai.revealed.set(p, ai.revealed_override[p] != 0);
            ai.view.revealed.set(p, ai.revealed_override[p] != 0);
        }
    };
    uint64_t checksum = 0xcbf29ce484222325ull;
    auto hash_matrices = [&](const gal::AllPairsMatrices& m) {
        const auto w = m.weights();
        const auto pr = m.preds();
        checksum = mix(checksum, w.data(), w.size_bytes());
        checksum = mix(checksum, pr.data(), pr.size_bytes());
    };
    // Node_Weight inputs from perception (published by this frame's update); replay overrides win.
    auto fill_values = [&](uint32_t a, std::vector<double>& v) {
        const gal::AllPairsInput in = perception.all_pairs_input(ai_player[a]);
        v.assign(n, 0.0);
        for (uint32_t p = 0; p < n && p < in.node_value.size(); ++p) v[p] = in.node_value[p];
        for (uint32_t p = 0; p < n; ++p)
            if (ais[a].has_override[p]) v[p] = ais[a].override_value[p];
    };

    // Game randomness: the original's synchronized + free LCG pair (alamo/core/sim_rng.hpp). The host
    // supplies the session seed; the sim never reads a clock.
    alamo::GameRandom rng(static_cast<uint32_t>(rng_seed), 0);

    alamo::jobs::TickScheduler sched(js);
    for (auto& ai : ais)
        if (ai.deferred) sched.track(*ai.deferred);

    // ---- replay commands: engine/commands pipeline, first system of every tick ----
    struct FleetCmd {
        uint32_t ai, src, dst;
        int mode;
    };
    std::vector<FleetCmd> queued;
    uint64_t batch_crc = 0;           // this tick's canonical batch CRC (pipeline hook)
    alamo::StateHasher batch_hist;    // running hash of every tick's batch CRC
    std::unique_ptr<cmd::CommandPipeline> pipeline;
    if (recording) {
        pipeline = std::make_unique<cmd::CommandPipeline>(recording->config);
        pipeline->replay_from(recording);
        pipeline->set_crc_hook([&](const void* data, size_t size) {
            alamo::StateHasher h;
            h.bytes(data, size);
            batch_crc = h.digest();
            batch_hist.u64(batch_crc);
        });
        pipeline->set_handler([&](uint64_t, const cmd::Command& c) -> cmd::RejectReason {
            const uint32_t a = c.header.player;
            if (a >= n_ais) return cmd::RejectReason::InvalidPlayer;
            if (c.payload.type() != cmd::EventType::PortExtension) return cmd::RejectReason::Unhandled;
            const uint32_t sub = c.payload.u("subtype");
            alamo::ByteReader r(c.payload.bytes_view("data"));
            if (sub == kRawFleetMove) {
                const uint32_t src = r.u32();
                const uint32_t dst = r.u32();
                const uint32_t mode = r.u8();
                if (!r.ok() || !r.done()) return cmd::RejectReason::Malformed;
                if (src == 0 || src > n || dst == 0 || dst > n || mode >= 6) return cmd::RejectReason::InvalidTarget;
                queued.push_back(FleetCmd{a, src - 1, dst - 1, static_cast<int>(mode)});
                return cmd::RejectReason::None;
            }
            if (sub == kRawSetPerception) {
                const uint32_t p = r.u32();
                const double v = r.f64();
                if (!r.ok() || !r.done()) return cmd::RejectReason::Malformed;
                if (p >= n || !std::isfinite(v)) return cmd::RejectReason::InvalidTarget;
                ais[a].has_override[p] = 1;
                ais[a].override_value[p] = v;
                return cmd::RejectReason::None;
            }
            if (sub == kRawSetRevealed) {
                const uint32_t p = r.u32();
                const uint32_t on = r.u8();
                if (!r.ok() || !r.done()) return cmd::RejectReason::Malformed;
                if (p >= n || on > 1) return cmd::RejectReason::InvalidTarget;
                ais[a].revealed_override[p] = static_cast<int8_t>(on);
                ais[a].revealed.set(p, on != 0);
                if (ais[a].view.revealed.size() == n) ais[a].view.revealed.set(p, on != 0);
                return cmd::RejectReason::None;
            }
            return cmd::RejectReason::Unhandled;
        });
        pipeline->events().subscribe([&](const cmd::Event& e) {
            if (const auto* cr = std::get_if<cmd::CommandResult>(&e)) {
                if (cr->accepted()) ++res.cmd_applied;
                else ++res.cmd_rejected;
            }
        });
    }
    sched.add_system("commands", 1, 0, [&](const alamo::jobs::TickContext& ctx) {
        queued.clear();
        if (!pipeline) return;
        pipeline->begin_tick(ctx.frame);
        pipeline->events().deliver();
        // Synced-RNG consumer (replay runs): every 15 frames one AI's view of one planet jitters.
        // Draws are separate statements: C++ argument evaluation order is unspecified.
        if (n > 0 && n_ais > 0 && ctx.frame % 15 == 7) {
            alamo::SimRng& r = rng.synced();
            const auto a = static_cast<uint32_t>(r.range_int(0, static_cast<int32_t>(n_ais) - 1));
            const auto p = static_cast<uint32_t>(r.range_int(0, static_cast<int32_t>(n) - 1));
            const int32_t v = r.range_int(-100, 900);
            ais[a].has_override[p] = 1;
            ais[a].override_value[p] = static_cast<double>(v);
        }
    });

    // S2 perception, once per frame before the AI services (GALACTIC_ECONOMY.md 6.3).
    sched.add_system("ai.perception", 1, 0, [&](const alamo::jobs::TickContext& ctx) {
        if (ctx.frame > 0 && ctx.frame % (2 * kRecomputePeriod) == 0)
            aip::perturb_snapshot(snap, snap_seed, static_cast<uint32_t>(ctx.frame));
        perception.update(snap, static_cast<uint32_t>(ctx.frame), pf);
        for (uint32_t a = 0; a < n_ais; ++a) refresh_ai(a);
    });

    // Q11 cadence: first recompute at creation (frame 0), next staggered by slot, then every 90.
    const auto k = static_cast<int32_t>(n_ais);
    sched.add_system("ai.all_pairs", 1, 0, [&](const alamo::jobs::TickContext& ctx) {
        const auto frame = static_cast<int32_t>(ctx.frame);
        for (uint32_t a = 0; a < n_ais; ++a) {
            Ai& ai = ais[a];
            if (ai.deferred && ai.kicks > 0 && ctx.frame == ai.deferred->ready_frame()) {
                hash_matrices(ai.deferred->read(ctx.frame));  // published exactly at kick + latency
            }
            if (!gal::recompute_due(frame, ai.next_recompute)) continue;
            // Background mode: a recompute that comes due while the previous one is still in
            // flight waits until that result is published (deterministic: depends on frames only).
            if (ai.deferred && ai.deferred->in_flight() && ctx.frame < ai.deferred->ready_frame()) continue;
            ai.next_recompute = ai.kicks == 0 ? gal::first_recompute_due(frame, static_cast<int32_t>(a), k)
                                              : gal::next_after_service(frame);
            const auto t = Clock::now();
            if (!ai.deferred) {
                fill_values(a, ai.value);
                gal::AllPairsInput in;
                in.node_value = ai.value;
                in.revealed = ai.fog ? &ai.revealed : nullptr;
                if (ai.cache.update(graph, in, pf)) {
                    ++res.recomputes;
                    const double dt = ms_since(t);
                    res.recompute_ms_total += dt;
                    res.recompute_ms_max = std::max(res.recompute_ms_max, dt);
                } else {
                    ++res.skipped;
                }
                hash_matrices(ai.cache.matrices());
            } else {
                // Background recompute: inputs snapshotted now (values AND fog mask, which replay
                // commands may change while the job runs), result visible at frame + latency.
                std::vector<double> values;
                fill_values(a, values);
                ai.deferred->kick(ctx.frame, opt.latency,
                                  [&graph, revealed = ai.revealed, fog = ai.fog, values = std::move(values),
                                   pf](gal::AllPairsMatrices& out) {
                                      gal::AllPairsInput in;
                                      in.node_value = values;
                                      in.revealed = fog ? &revealed : nullptr;
                                      gal::compute_all_pairs(graph, in, out, pf);
                                  });
                ++res.recomputes;
            }
            ++ai.kicks;
        }
    });
    // Per-tick fleet results (hashed by the fleet_orders CRC system).
    std::vector<alamo::PlanetIndex> tick_paths;
    std::vector<uint32_t> tick_path_len;
    std::vector<float> tick_costs;
    sched.add_system("fleet_orders", 1, 0, [&](const alamo::jobs::TickContext& ctx) {
        tick_paths.clear();
        tick_path_len.clear();
        tick_costs.clear();
        if (n < 2) return;
        // Each AI issues a handful of fleet moves per frame, scored in parallel and committed
        // in order (the AI goal-proposal shape); replay FleetMove commands follow.
        const uint32_t kPerFrame = 8;
        const uint32_t random_count = n_ais * kPerFrame;
        struct Out {
            std::vector<alamo::PlanetIndex> path;
            float cost = 0;
            bool ok = false;
        };
        const auto t = Clock::now();
        alamo::jobs::parallel_map_then_ordered_commit(
            js, random_count + static_cast<uint32_t>(queued.size()), 1,
            [&](uint32_t i) {
                Out o;
                thread_local gal::PathScratch scratch;
                if (i >= random_count) {
                    const FleetCmd& c = queued[i - random_count];
                    const auto req = gal::PathRequest::for_ai_mode(c.src, c.dst, c.mode);
                    o.ok = gal::find_fleet_path(graph, ais[c.ai].view, req, scratch, o.path, &o.cost);
                    return o;
                }
                const uint32_t a = i / kPerFrame;
                Lcg r(static_cast<uint32_t>(ctx.frame * 2654435761u + i));
                // Draw in a fixed order (argument evaluation order is unspecified in C++).
                const auto src = r.next() % n;
                const auto dst = r.next() % n;
                const auto mode = static_cast<int>(r.next() % 6);
                const auto req = gal::PathRequest::for_ai_mode(src, dst, mode);
                o.ok = gal::find_fleet_path(graph, ais[a].view, req, scratch, o.path, &o.cost);
                return o;
            },
            [&](uint32_t, Out& o) {
                ++res.paths;
                if (o.ok) ++res.reachable;
                checksum = mix(checksum, o.path.data(), o.path.size() * sizeof(alamo::PlanetIndex));
                checksum = mix(checksum, &o.cost, sizeof o.cost);
                tick_path_len.push_back(static_cast<uint32_t>(o.path.size()));
                tick_paths.insert(tick_paths.end(), o.path.begin(), o.path.end());
                tick_costs.push_back(o.cost);
            });
        res.astar_ms_total += ms_since(t);
    });

    // ---- Q1 CRC systems: canonical state after each tick (contract: alamo/core/sim_crc.hpp) ----
    alamo::CrcRegistry reg;
    if (crc_log) {
        reg.add("galaxy.graph", [&](alamo::StateHasher& h) {
            h.u32(n);
            h.u64(graph.fingerprint());
            h.span(graph.offsets());
            h.span(graph.neighbours());
        });
        reg.add("commands", [&](alamo::StateHasher& h) {
            h.u32(res.cmd_applied);
            h.u32(res.cmd_rejected);
            h.u64(batch_crc);
            h.u64(batch_hist.digest());
            h.u32(static_cast<uint32_t>(queued.size()));
        });
        reg.add("rng", [&](alamo::StateHasher& h) { rng.hash(h); });
        // Retail-shaped parity CRC: the sync-RNG term (objects and players join when they exist).
        reg.add_parity("rng", [&] { return alamo::parity_rng_crc(rng.synced().state()); });
        aip::register_crc(reg, perception);  // "ai.perception"
        reg.add("ai.state", [&](alamo::StateHasher& h) {
            h.u32(static_cast<uint32_t>(ais.size()));
            for (const Ai& ai : ais) {
                h.i32(ai.next_recompute);
                h.u32(ai.kicks);
                h.boolean(ai.fog);
                h.span(ai.revealed_override);
                h.u32(static_cast<uint32_t>(ai.revealed.size()));
                h.span(ai.revealed.words());
                h.span(ai.has_override);
                h.span(ai.override_value);
                const gal::AllPairsMatrices& m = ai.deferred ? ai.deferred->current() : ai.cache.matrices();
                h.u32(m.size());
                h.span(m.weights());
                h.span(m.preds());
                if (ai.deferred) {
                    h.u64(ai.deferred->version());
                    h.boolean(ai.deferred->in_flight());
                    h.u64(ai.deferred->ready_frame());
                }
            }
        });
        reg.add("fleet_orders", [&](alamo::StateHasher& h) {
            h.u32(res.paths);
            h.u32(res.reachable);
            h.u32(static_cast<uint32_t>(tick_path_len.size()));
            h.span(tick_path_len);
            h.span(tick_paths);
            h.span(tick_costs);
        });
        crc_log->systems = reg.names();
        crc_log->parity_parts = reg.parity_names();
        crc_log->records.clear();
    }

    t0 = Clock::now();
    for (uint32_t f = 0; f < opt.frames; ++f) {
        sched.step();
        if (crc_log) crc_log->records.push_back(reg.capture(f));
    }
    res.run_ms = ms_since(t0);
    res.checksum = checksum;
    return res;
}

void print_result(const RunResult& r, int32_t workers) {
    std::printf("%-40s planets %4u routes %4u edges %5u | graph %.2f ms | all-pairs %u recompute (avg %.3f ms, max %.3f ms), %u cache hits | "
                "A* %u (%u reachable) %.3f ms total | run %.1f ms | workers %d | checksum %016" PRIx64 "\n",
                r.name.c_str(), r.planets, r.routes, r.edges, r.graph_ms, r.recomputes,
                r.recomputes ? r.recompute_ms_total / r.recomputes : 0.0, r.recompute_ms_max, r.skipped, r.paths,
                r.reachable, r.astar_ms_total, r.run_ms, workers, r.checksum);
}

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_headless (--data DIR | --game DIR [MODPATH=..] [FALLBACKPATH=..] | --synthetic N | --selfcheck)\n"
                 "                      [--campaign NAME | --list | --all] [--frames N] [--workers N] [--ais K] [--latency F]\n"
                 "       alamo_headless --replay STREAM [--workers N] [--frames N] [--crc-out FILE] [--golden FILE] [--check-workers]\n"
                 "       alamo_headless --record-stream FILE [--seed S] [--frames N] [--synthetic P] [--ais K] [--latency F]\n"
                 "       alamo_headless --compare EXPECTED.crc ACTUAL.crc\n"
                 "       alamo_headless --crc-convert IN.crc OUT.crc [--crc-checkpoint N] [--crc-keep T,T,...]\n"
                 "       (any run with --crc-out FILE also takes --crc-checkpoint N [--crc-keep T,T,...]: write the checkpoint log)\n"
                 "       alamo_headless --map NAME [--game DIR] [--ticks N] [--workers W] [--seed S] [--no-ai] [--realtime]\n"
                 "                      [--record-orders FILE | --replay FILE] [--crc-out FILE] [--golden FILE] [--check-workers]\n"
                 "       alamo_headless --economy | --story (--game DIR | --data DIR) [--campaign NAME] [--ticks N] [--workers W]\n"
                 "                      [--seed S] [--check-workers] [--story-out FILE] [--story-golden FILE] [--crc-out FILE]\n"
                 "                      [--scoring] (run the GameScoring script at the session start, as the game does)\n"
                 "                      [--no-object-scripts] (--skirmish-ai: game-object Lua scripts and the GameScoring\n"
                 "                      script run by default, as the game does; this turns both off. OBJECT_SCRIPTS.md)\n"
                 "       alamo_headless --economy --battles auto|fight|fight-all [--aggressor FRAMES] [--battle-retreat R]\n"
                 "                      [--battle-max-frames N] (+ the --economy options)\n"
                 "       alamo_headless --save-session synthetic|hotm|galactic|skirmish [--campaign NAME] --save-at T FILE [--ticks K]\n"
                 "         [--battles auto|fight|fight-all[+anim]] [--story] [--map M] [--save-when any|battle|galactic|pending|scheduled]\n"
                 "                      [--crc-out F] [--record-orders F] [--seed S] [--workers W]\n"
                 "       alamo_headless --load FILE [--ticks K] [--crc-out F] [--replay STREAM] [--record-orders F] [--workers W]\n"
                 "       alamo_headless --save-session KIND --save-check --save-at T FILE --ticks K\n"
                 "       alamo_headless --bench SCENARIO.abench [--runs R] [--warmup W] [--workers N] [--ticks K] [--bench-json FILE|-]\n"
                 "                      [--bench-baseline OLD.json] [--crc-out F] [--golden F] [--bench-allow-changed]\n"
                 "       alamo_headless --bench-make SCENARIO.abench --save-session KIND [--campaign C] [--map M] [--battles B] [--story]\n"
                 "                      [--seed S] --bench-save-at T [--save-when W] --ticks K [--workers N] [--bench-every N] [--bench-desc TEXT]\n"
                 "       alamo_headless --bench-regen SCENARIO.abench\n"
                 "       (any run also takes --squadron-size N: fighter / bomber squadron members, 0 = retail lists and prices,\n"
                 "        default 12 [port: Gabriel]; the parity harness and the retail-outcome tests pass 0)\n");
    return 2;
}

// ---- Q1 modes ------------------------------------------------------------------------------------
uint32_t meta_u32(const cmd::CommandRecording& rec, const char* key, uint32_t fallback) {
    for (const auto& [k, v] : rec.meta)
        if (k == key) return static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    return fallback;
}

// Records a seeded random command stream through the real pipeline (submit -> drain -> record).
int record_stream_mode(const Options& opt) {
    const uint64_t ticks = opt.frames_set ? opt.frames : 600;
    const uint32_t planets = opt.synthetic ? opt.synthetic : 120;
    const uint32_t ais = opt.ais_set ? opt.ais : 3;
    cmd::SchedulerConfig cfg;
    cfg.delay = 1;
    cfg.max_players = 8;
    cfg.first_tick = 0;
    cmd::CommandPipeline p(cfg);
    cmd::CommandRecording rec;
    p.record_to(&rec);
    alamo::SimRng r(static_cast<uint32_t>(opt.seed) ^ 0xC0DEu);
    auto below = [&r](uint32_t k) { return static_cast<uint32_t>(r.range_int(0, static_cast<int32_t>(k) - 1)); };
    for (uint64_t t = 0; t < ticks; ++t) {
        p.begin_tick(t);  // drains (and records) what was submitted during the previous tick
        if (t + 1 == ticks) break;
        const uint32_t count = below(4);
        for (uint32_t c = 0; c < count; ++c) {
            const uint32_t kind = below(20);
            auto player = static_cast<uint8_t>(below(ais));
            alamo::ByteWriter w;
            uint16_t sub = kRawFleetMove;
            if (kind < 6) {
                const uint32_t pl = below(planets);
                const int32_t v = r.range_int(-200, 1000);
                w.u32(pl).f64(static_cast<double>(v) * 0.5);
                sub = kRawSetPerception;
            } else if (kind < 11) {
                const uint32_t pl = below(planets);
                const uint32_t on = below(2);
                w.u32(pl).u8(static_cast<uint8_t>(on));
                sub = kRawSetRevealed;
            } else if (kind < 18) {
                const uint32_t src = below(planets);
                const uint32_t dst = below(planets);
                const uint32_t mode = below(6);
                w.u32(src + 1).u32(dst + 1).u8(static_cast<uint8_t>(mode));
            } else if (kind == 18 && ais < 8) {  // invalid: player slot without an AI
                player = static_cast<uint8_t>(ais + below(8 - ais));
                w.u32(1).u32(2).u8(0);
            } else {  // invalid: unknown subtype or truncated payload
                sub = below(2) ? uint16_t{0x8099} : kRawSetPerception;
                w.u8(1).u8(2).u8(3);
            }
            cmd::EventData payload = cmd::ev::port_extension(sub, w.bytes());
            const auto sr = p.submit(player, cmd::CommandSource::AI, std::move(payload));
            if (!sr.ok()) {
                std::fprintf(stderr, "record: submit failed (%u)\n", static_cast<unsigned>(sr.status));
                return 1;
            }
        }
    }
    rec.seed = opt.seed;
    rec.meta = {{"galaxy", "synthetic"},
                {"planets", std::to_string(planets)},
                {"ais", std::to_string(ais)},
                {"latency", std::to_string(opt.latency)}};
    std::string err;
    if (!rec.save(opt.record_stream, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s: %zu commands in %zu batches over %" PRIu64 " ticks (seed %" PRIu64 ")\n",
                opt.record_stream.c_str(), rec.command_count(), rec.batches.size(), rec.end_tick, rec.seed);
    return 0;
}

int compare_mode(const Options& opt) {
    alamo::CrcLog a, b;
    std::string err;
    if (!alamo::CrcLog::load(opt.compare_a, a, &err) || !alamo::CrcLog::load(opt.compare_b, b, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const auto d = alamo::first_divergence(a, b);
    std::printf("%s: %s\n", d.diverged ? "DIVERGED" : "MATCH", d.message.c_str());
    return d.diverged ? 1 : 0;
}

int convert_mode(const Options& opt) {
    alamo::CrcLog log;
    std::string err;
    if (!alamo::CrcLog::load(opt.convert_in, log, &err)) {
        std::fprintf(stderr, "%s: %s\n", opt.convert_in.c_str(), err.c_str());
        return 2;
    }
    const alamo_headless::CrcOutSpec& spec = alamo_headless::crc_out_spec();
    const alamo::CrcLog out = log.checkpointed(spec.checkpoint != 0 ? spec.checkpoint : 64u, spec.keep);
    if (!out.save(opt.convert_out, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::printf("wrote %s (%zu of %zu records, checkpoint %u)\n", opt.convert_out.c_str(), out.records.size(), log.records.size(),
                out.checkpoint);
    return 0;
}

int replay_mode(const Options& opt) {
    cmd::CommandRecording rec;
    std::string err;
    if (!cmd::CommandRecording::load(opt.replay, rec, &err)) {
        std::fprintf(stderr, "replay: %s\n", err.c_str());
        return 2;
    }
    Options o = opt;
    const uint32_t planets = meta_u32(rec, "planets", 120);
    o.ais = meta_u32(rec, "ais", 3);
    o.latency = meta_u32(rec, "latency", alamo::galactic::kDefaultAllPairsLatency);
    o.frames = opt.frames_set ? opt.frames : static_cast<uint32_t>(rec.end_tick ? rec.end_tick : 600);
    const auto map = make_synthetic(planets, static_cast<uint32_t>(rec.seed));
    const std::string stream_name = std::filesystem::path(opt.replay).filename().string();
    auto run_one = [&](int32_t w, alamo::CrcLog& log) {
        const RunResult r = run_campaign(map, o, w, &rec, &log, rec.seed);
        log.comments = {"replay " + stream_name + " seed " + std::to_string(rec.seed) + " ticks " +
                            std::to_string(o.frames) + " planets " + std::to_string(planets) + " ais " +
                            std::to_string(o.ais) + " latency " + std::to_string(o.latency),
                        "commands applied " + std::to_string(r.cmd_applied) + " rejected " + std::to_string(r.cmd_rejected)};
        const uint64_t final_crc = log.records.empty() ? 0 : log.records.back().combined;
        std::printf("replay %s workers %d: %u ticks, %u commands applied, %u rejected, final CRC %016" PRIx64
                    ", %.1f ms\n",
                    stream_name.c_str(), w, o.frames, r.cmd_applied, r.cmd_rejected, final_crc, r.run_ms);
        return r;
    };

    bool ok = true;
    alamo::CrcLog log;
    if (opt.check_workers) {
        auto rep = alamo::jobs::check_worker_counts([&](int32_t w) {
            alamo::CrcLog l;
            run_one(w, l);
            return l;
        });
        for (size_t i = 1; i < rep.runs.size(); ++i)
            std::printf("  workers %d vs %d: %s\n", rep.runs.front().workers, rep.runs[i].workers, rep.runs[i].vs_first.message.c_str());
        ok = rep.ok();
        log = std::move(rep.first);
    } else {
        run_one(opt.workers, log);
    }
    if (!opt.crc_out.empty()) {
        if (!alamo_headless::save_crc_log(log, opt.crc_out, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        std::printf("wrote %s (%zu records)\n", opt.crc_out.c_str(), log.records.size());
    }
    if (!opt.golden.empty() && alamo_headless::compare_golden(opt.golden, log, ok, true) != 0) return 2;
    std::printf(ok ? "replay: OK\n" : "replay: FAILED\n");
    return ok ? 0 : 1;
}

}  // namespace

static int headless_main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--data") opt.data_dir = next();
        else if (a == "--game") opt.game_dir = next();
        else if (a == "--campaign") opt.campaign = next();
        else if (a == "--frames") {
            opt.frames = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
            opt.frames_set = true;
        } else if (a == "--workers") {
            opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
            opt.workers_set = true;
        }
        else if (a == "--ais") {
            opt.ais = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
            opt.ais_set = true;
        } else if (a == "--latency") opt.latency = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--synthetic") opt.synthetic = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--list") opt.list = true;
        else if (a == "--all") opt.all = true;
        else if (a == "--selfcheck") opt.selfcheck = true;
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--replay") opt.replay = next();
        else if (a == "--crc-out") opt.crc_out = next();
        else if (a == "--golden") opt.golden = next();
        else if (a == "--crc-checkpoint") alamo_headless::crc_out_spec().checkpoint = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--crc-keep") {
            const std::string list = next();
            size_t i0 = 0;
            while (i0 <= list.size()) {
                const size_t e = std::min(list.find(',', i0), list.size());
                if (e > i0) alamo_headless::crc_out_spec().keep.push_back(std::strtoull(list.substr(i0, e - i0).c_str(), nullptr, 10));
                i0 = e + 1;
            }
        } else if (a == "--crc-convert") {
            opt.convert_in = next();
            opt.convert_out = next();
        }
        else if (a == "--check-workers") opt.check_workers = true;
        else if (a == "--record-stream") opt.record_stream = next();
        else if (a == "--seed") {
            opt.seed = std::strtoull(next().c_str(), nullptr, 10);
            opt.seed_set = true;
        }
        else if (a == "--map") opt.map = next();
        else if (a == "--record-orders") opt.record_orders = next();
        else if (a == "--ticks") {
            opt.ticks = std::strtoull(next().c_str(), nullptr, 10);
            opt.ticks_set = true;
        } else if (a == "--realtime") opt.realtime = true;
        else if (a == "--no-ai") opt.no_ai = true;
        else if (a == "--economy") opt.economy = true;
        else if (a == "--story") opt.story = true;
        else if (a == "--scoring") opt.scoring = true;  // q6 t06
        else if (a == "--object-scripts") opt.object_scripts = true;  // q6 (the default since q6 t23)
        else if (a == "--no-object-scripts") opt.object_scripts = false;  // q6 t23
        else if (a == "--control-points") opt.control_points = true;  // ccpgt t02
        else if (a == "--story-out") opt.story_out = next();
        else if (a == "--story-golden") opt.story_golden = next();
        else if (a == "--battles") opt.battles = next();  // S10
        else if (a == "--galactic-ai") opt.galactic_ai = true;  // A2
        else if (a == "--skirmish-ai") opt.skirmish_ai = next();  // A1 stage 2
        else if (a == "--ai") opt.skirmish_factions = next();     // q6 t02
        else if (a == "--expect-battle-end") opt.expect_battle_end = true;  // S10 land outcome
        else if (a == "--expect-skirmish-base") opt.expect_skirmish_base = true;  // t50
        else if (a == "--fail-on-lua-errors") opt.fail_on_lua_errors = true;  // a1j t07
        else if (a == "--squadron-size") {  // b0319: 0 = retail squadrons (the parity harness), else members per squadron
            const unsigned long n = std::strtoul(next().c_str(), nullptr, 10);
            if (n > 64) return usage();
            alamo::objects::GameObjectTypes::set_default_squadron_size(static_cast<uint32_t>(n));
        }
        else if (a == "--ai-log") opt.ai_log = next();
        else if (a == "--ai-golden") opt.ai_golden = next();
        else if (a == "--human") opt.ai_human = next();                // A2 galactic
        else if (a == "--human-script") opt.ai_human_script = next();  // A2 galactic
        else if (a == "--ai-type") opt.ai_types.push_back(next());        // q6 t07
        else if (a == "--trace-out") opt.trace_out = next();            // q2f t05
        else if (a == "--ai-latency") opt.ai_latency = std::strtoll(next().c_str(), nullptr, 10);  // OPT2 t02
        else if (a == "--no-ai-stagger") opt.ai_stagger = 0;                                       // OPT2 t01
        else if (a == "--ai-stagger") opt.ai_stagger = 1;                                          // a2m r4
        else if (a == "--no-pp-streams") opt.pp_streams = false;                                   // OPT2 t10
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        else if (a == "--ai-lane") alamo::sim::ai_lane_defaults().on = true;  // sp3
        else if (a == "--ai-lane-k") alamo::sim::ai_lane_defaults().k_ai = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
#endif
        else if (a == "--pp-latency") opt.pp_latency = std::strtoll(next().c_str(), nullptr, 10);   // OPT2 t10
        else if (a == "--aggressor") opt.aggressor = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--battle-retreat") opt.battle_retreat = static_cast<float>(std::strtod(next().c_str(), nullptr));
        else if (a == "--battle-max-frames") opt.battle_max_frames = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--save-session") opt.save_session = next();  // N1
        else if (a == "--save-when") opt.save_when = next();  // N1 stage 2
        else if (a == "--save-at") {
            opt.save_at = std::strtoull(next().c_str(), nullptr, 10);
            opt.save_at_set = true;
            opt.save_file = next();
        } else if (a == "--load") opt.load_file = next();
        else if (a == "--save-check") opt.save_check = true;
        else if (a == "--bench") opt.bench = next();  // BM1
        else if (a == "--bench-make") opt.bench_make = next();
        else if (a == "--bench-regen") opt.bench_regen = next();
        else if (a == "--bench-json") opt.bench_json = next();
        else if (a == "--bench-baseline") opt.bench_baseline = next();
        else if (a == "--bench-desc") opt.bench_desc = next();
        else if (a == "--bench-allow-changed") opt.bench_allow_changed = true;
        else if (a == "--runs") opt.bench_runs = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--warmup") opt.bench_warmup = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--bench-every") opt.bench_every = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--bench-save-at") {
            opt.bench_save_at = std::strtoull(next().c_str(), nullptr, 10);
            opt.bench_save_at_set = true;
        } else if (a == "--compare") {
            opt.compare_a = next();
            opt.compare_b = next();
        } else if (a.find('=') != std::string::npos) {
            opt.game_args.push_back(a);  // MODPATH= etc.
            opt.game_arg_index.push_back(i);
        } else return usage();
    }

#ifdef ALAMO_HEADLESS_HAVE_SIM
    if (!opt.bench_regen.empty()) {  // BM1: remake a scenario with the arguments its manifest recorded
        alamo::sim::bench::Scenario sc;
        std::string err;
        if (!alamo::sim::bench::Scenario::load(opt.bench_regen, sc, &err)) {
            std::fprintf(stderr, "bench-regen: %s\n", err.c_str());
            return 1;
        }
        if (sc.host == alamo::sim::bench::Host::Game) {  // BM2: alamo_game remakes its own (alamo_game --bench-make)
            std::fprintf(stderr, "bench-regen: %s was made by alamo_game (host game): remake it with alamo_game --bench-make\n",
                         opt.bench_regen.c_str());
            return 2;
        }
        if (sc.make_args.empty()) {
            std::fprintf(stderr, "bench-regen: %s records no make arguments\n", opt.bench_regen.c_str());
            return 2;
        }
        std::vector<std::string> args = {argc > 0 ? argv[0] : "alamo_headless", "--bench-make", opt.bench_regen};
        args.insert(args.end(), sc.make_args.begin(), sc.make_args.end());
        if (!opt.game_dir.empty()) {
            args.push_back("--game");
            args.push_back(opt.game_dir);
        }
        args.insert(args.end(), opt.game_args.begin(), opt.game_args.end());
        if (opt.quiet) args.push_back("--quiet");
        std::vector<char*> av;
        for (std::string& x : args) av.push_back(x.data());
        std::printf("bench-regen: %s version %u, make", opt.bench_regen.c_str(), sc.version);
        for (const std::string& x : sc.make_args) std::printf(" %s", x.c_str());
        std::printf("\n");
        return headless_main(static_cast<int>(av.size()), av.data());
    }
#endif
    if (!opt.bench.empty() || !opt.bench_make.empty()) {  // BM1 benchmark
#ifdef ALAMO_HEADLESS_HAVE_SIM
        BenchOptions b;
        b.scenario = opt.bench.empty() ? opt.bench_make : opt.bench;
        b.runs = opt.bench_runs;
        b.warmup = opt.bench_warmup;
        b.workers = opt.workers;
        b.workers_set = opt.workers_set;
        b.ticks = opt.ticks;
        b.ticks_set = opt.ticks_set;
        b.json_out = opt.bench_json;
        b.baseline = opt.bench_baseline;
        b.crc_out = opt.crc_out;
        b.golden = opt.golden;
        b.allow_changed = opt.bench_allow_changed;
        b.quiet = opt.quiet;
        b.game_dir = opt.game_dir;
        b.game_args = opt.game_args;
        if (opt.bench.empty()) {
            b.make.session = opt.save_session;
            b.make.campaign = opt.campaign;
            b.make.map = opt.map;
            b.make.battles = opt.battles;
            b.make.story = opt.story;
            b.make.seed = static_cast<uint32_t>(opt.seed_set ? opt.seed : 7u);
            b.make.seed_set = opt.seed_set;
            b.make.save_at = opt.bench_save_at;
            b.make.save_at_set = opt.bench_save_at_set;
            b.make.save_when = opt.save_when;
            b.make.quiet = opt.quiet;
            b.fingerprint_every = opt.bench_every;
            b.description = opt.bench_desc;
            // The manifest's "make" line: this command's arguments without the output path and the machine-specific ones.
            for (int i = 1; i < argc; ++i) {
                const std::string a = argv[i];
                if ((a == "--bench-make" || a == "--game" || a == "--data") && i + 1 < argc) ++i;
                else if (a == "--quiet" || std::find(opt.game_arg_index.begin(), opt.game_arg_index.end(), i) != opt.game_arg_index.end()) continue;
                else b.make_args.push_back(a);
            }
            return bench_make(b);
        }
        return bench_run(b);
#else
        std::fprintf(stderr, "--bench: this build has no alamo_sim\n");
        return 2;
#endif
    }
    if (!opt.save_session.empty() || !opt.load_file.empty()) {  // N1 save / load
#ifdef ALAMO_HEADLESS_HAVE_SIM
        SaveRunOptions s;
        s.session = opt.save_session;
        s.campaign = opt.campaign;
        s.save_file = opt.save_file;
        s.save_at = opt.save_at;
        s.save_at_set = opt.save_at_set;
        s.load_file = opt.load_file;
        s.check = opt.save_check;
        s.ticks = opt.ticks_set ? opt.ticks : (opt.save_session.empty() || opt.save_check ? 600 : 0);
        s.crc_out = opt.crc_out;
        s.replay = opt.replay;
        s.record_orders = opt.record_orders;
        s.game_dir = opt.game_dir;
        s.game_args = opt.game_args;
        s.workers = opt.workers;
        s.seed = static_cast<uint32_t>(opt.seed_set ? opt.seed : 7u);
        s.seed_set = opt.seed_set;
        s.quiet = opt.quiet;
        s.self = argc > 0 ? argv[0] : "alamo_headless";
        s.battles = opt.battles;  // N1 stage 2
        s.story = opt.story;
        s.map = opt.map;
        s.save_when = opt.save_when;
        return save_run(s);
#else
        std::fprintf(stderr, "--save-session / --load: this build has no alamo_sim\n");
        return 2;
#endif
    }
    if (!opt.map.empty() || opt.economy || opt.story || !opt.skirmish_ai.empty()) {
#ifdef ALAMO_HEADLESS_HAVE_SIM
        MapRunOptions m;
        m.map = (opt.economy || opt.story) ? opt.campaign : opt.map;
        m.game_dir = opt.game_dir;
        m.game_args = opt.game_args;
        m.ticks = opt.ticks;
        m.ticks_set = opt.ticks_set;
        m.workers = opt.workers;
        m.seed = static_cast<uint32_t>(opt.seed);
        m.record_orders = opt.record_orders;
        m.replay = opt.replay;
        m.crc_out = opt.crc_out;
        m.golden = opt.golden;
        m.check_workers = opt.check_workers;
        m.realtime = opt.realtime;
        m.ai = !opt.no_ai;
        m.quiet = opt.quiet;
        m.ai_latency = opt.ai_latency;  // OPT2 t01 / t02 parity knobs
        m.ai_stagger = opt.ai_stagger;
        m.pp_streams = opt.pp_streams;  // OPT2 t10 parity knobs
        m.pp_latency = opt.pp_latency;
        m.scoring = opt.scoring;  // q6 t06
        m.object_scripts = opt.object_scripts;  // q6
        if (!opt.skirmish_ai.empty()) {  // A1 stage 2: --skirmish-ai synthetic|foc [--map NAME]
            m.map = opt.map;
            m.expect_battle_end = opt.expect_battle_end;
            m.expect_skirmish_base = opt.expect_skirmish_base;
            m.human = opt.ai_human;  // skai: --skirmish-ai skirmish --human FACTION
            m.ai_factions = opt.skirmish_factions;  // q6 t02
            m.fail_on_lua_errors = opt.fail_on_lua_errors;
            m.control_points = opt.control_points;  // ccpgt t02
            return skirmish_ai_run(m, opt.skirmish_ai, opt.ai_log, opt.trace_out);  // q4hf t02: --trace-out
        }
        if (opt.story && !(opt.economy && opt.galactic_ai)) {
            if (!opt.ticks_set && opt.frames_set) m.ticks = opt.frames;
            return story_run(m, opt.data_dir, opt.story_out, opt.story_golden);
        }
        if (opt.economy && opt.galactic_ai) {  // A2
            if (!opt.ticks_set && opt.frames_set) m.ticks = opt.frames;
            AiRunOptions a;
            if (!opt.battles.empty()) a.battles = opt.battles;
            a.ai_log = opt.ai_log;
            a.ai_golden = opt.ai_golden;
            a.human = opt.ai_human;                // A2 galactic
            a.human_script = opt.ai_human_script;  // A2 galactic
            a.ai_types = opt.ai_types;             // q6 t07
            a.trace_out = opt.trace_out;           // q2f t05
            a.story = opt.story;                   // q2f t03
            a.verbose = !opt.quiet;
            return ai_campaign_run(m, a);
        }
        if (opt.economy && !opt.battles.empty()) {  // S10
            if (!opt.ticks_set && opt.frames_set) m.ticks = opt.frames;
            BattleRunOptions b;
            b.mode = opt.battles;
            b.aggressor_period = opt.aggressor;
            b.retreat_ratio = opt.battle_retreat;
            b.max_frames = opt.battle_max_frames;
            b.verbose = !opt.quiet;
            return battle_run(m, opt.data_dir, b);
        }
        if (opt.economy) {
            if (!opt.ticks_set && opt.frames_set) m.ticks = opt.frames;
            return economy_run(m, opt.data_dir);
        }
        return map_run(m);
#else
        std::fprintf(stderr, "--map: this build has no alamo_sim (objects/commands/tactical/terrain modules)\n");
        return 2;
#endif
    }
    if (!opt.compare_a.empty()) return compare_mode(opt);
    if (!opt.convert_in.empty()) return convert_mode(opt);
    if (!opt.record_stream.empty()) return record_stream_mode(opt);
    if (!opt.replay.empty()) return replay_mode(opt);

    if (opt.selfcheck) {
        // Determinism across worker counts on a mod-scale synthetic galaxy, for vanilla timing
        // (latency 0), the default background latency and background recomputes published 30 frames
        // later. Every tick's CRC record (not just the final checksum) must match the serial run.
        const auto map = make_synthetic(220, 12345);
        bool ok = true;
        for (uint32_t lat : {0u, alamo::galactic::kDefaultAllPairsLatency, 30u}) {
            Options o = opt;
            o.frames = 360;
            o.ais = 3;
            o.latency = lat;
            uint64_t first = 0;
            alamo::CrcLog first_log;
            for (int32_t w : alamo::jobs::determinism_worker_counts()) {
                alamo::CrcLog log;
                const auto r = run_campaign(map, o, w, nullptr, &log);
                std::printf("latency %2u: ", lat);
                print_result(r, w);
                if (w == 0) {
                    first = r.checksum;
                    first_log = std::move(log);
                } else {
                    if (r.checksum != first) ok = false;
                    const auto d = alamo::first_divergence(first_log, log);
                    std::printf("            per-tick CRCs vs serial: %s\n", d.message.c_str());
                    if (d.diverged) ok = false;
                }
                if (r.recomputes == 0 || r.paths == 0 || r.reachable == 0) ok = false;
                if (lat == 0 && r.skipped == 0) ok = false;
            }
        }
        std::printf(ok ? "selfcheck: OK (identical checksums and per-tick CRCs for 0/1/4/N workers, latency 0, 15 and 30)\n"
                       : "selfcheck: FAILED\n");
        return ok ? 0 : 1;
    }

    if (opt.synthetic) {
        const auto map = make_synthetic(opt.synthetic, 12345);
        print_result(run_campaign(map, opt, opt.workers), opt.workers);
        return 0;
    }

    // Game data through the VFS (real install) or a loose folder (extracted text).
    std::unique_ptr<alamo::FileProvider> provider;
    if (!opt.game_dir.empty()) {
        std::string err;
        auto v = alamo::vfs::mount_game(opt.game_dir, opt.game_args, &err);
        if (!v) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (!opt.quiet) std::printf("%s\n", v->describe().c_str());
        provider = std::move(v);
    } else if (!opt.data_dir.empty()) {
        provider = std::make_unique<alamo::DirectoryProvider>(opt.data_dir);
    } else {
        return usage();
    }

    auto t0 = Clock::now();
    alamo::data::GameDatabase db;
    std::vector<std::string> diag;
    const bool loaded = db.load(*provider, {}, &diag);
    std::printf("database: %s in %.1f ms - %zu campaigns, %zu planet types, %zu trade routes, %zu diagnostics\n",
                loaded ? "loaded" : "PARTIAL", ms_since(t0), db.campaigns().size(), db.planets().size(),
                db.trade_routes().size(), diag.size());
    if (!opt.quiet)
        for (const auto& d : diag) std::printf("  %s\n", d.c_str());

    if (opt.list) {
        for (const auto& c : db.campaigns()) std::printf("  %s\n", c.name.c_str());
        return 0;
    }

    std::vector<std::string> names;
    if (opt.all) {
        for (const auto& c : db.campaigns()) names.push_back(c.name);
    } else if (!opt.campaign.empty()) {
        names.push_back(opt.campaign);
    } else if (!db.campaigns().empty()) {
        names.push_back(db.campaigns().front().name);
    }

    // S2 perception seed: the campaign's starting state, with force types from the typed object
    // database (engine/objects).
    alamo::objects::ObjectTypeDatabase odb;
    alamo::objects::GameObjectTypes types;
    const bool have_types = odb.load(*provider) && types.build(odb, provider.get());
    const alamo::ai::ForceTypeLookup lookup = alamo::ai::make_force_type_lookup(types);
    // modc t03: Is_Neutral / Benefits_From_Corruption from FactionFiles.xml (FoC: Neutral / Underworld, as before).
    const alamo::ai::CampaignSeedOptions seed_options =
        alamo::ai::seed_options_from_factions(alamo::galactic::load_factions(*provider));
    int rc = 0;
    for (const auto& name : names) {
        std::vector<std::string> mdiag;
        const auto map = alamo::data::build_galaxy_map(db, name, &mdiag);
        if (map.planets.size() == 0) {
            std::printf("%s: no planets (%zu diagnostics)\n", name.c_str(), mdiag.size());
            rc = 1;
            continue;
        }
        const alamo::data::CampaignDef* camp = nullptr;
        for (const auto& c : db.campaigns())
            if (c.name == name) camp = &c;
        if (!camp || !have_types) {
            print_result(run_campaign(map, opt, opt.workers), opt.workers);  // synthetic perception snapshot
            continue;
        }
        const alamo::ai::CampaignSeed seed = alamo::ai::seed_campaign_snapshot(*camp, map, lookup, seed_options);
        Options o = opt;
        if (!opt.ais_set) o.ais = static_cast<uint32_t>(seed.snapshot.player_count());  // every AI player
        print_result(run_campaign(map, o, opt.workers, nullptr, nullptr, 0, &seed.snapshot), opt.workers);
        if (!opt.quiet)
            for (const auto& d : mdiag) std::printf("  %s\n", d.c_str());
    }
    return rc;
}

// OPT1: --profile [--profile-out FILE] [--profile-budget MS] times every tick of the run (SimHost / TickScheduler ticks)
// and prints p50 / p99 / max tick time, per-zone shares and the 20 worst ticks after it. Wall-clock data only: the run's
// results and CRCs are unchanged. Runs that tick several hosts in turn (--selfcheck, --check-workers) report them together.
int main(int argc, char** argv) {
    alamo::vfs::install_steam_workshop_lookup();  // modc t10: STEAMMOD= from the Steam library folders
    (void)alamo::jobs::disable_power_throttling();  // SIMP sp1: wall clock only (the sim thread's priority: bench_run)
    // Q5: an uncaught exception (e.g. mod data past a fixed limit) names itself instead of a silent 0xC0000409 exit.
    std::set_terminate([] {
        try {
            if (const std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& x) {
            std::fprintf(stderr, "alamo_headless: uncaught exception: %s\n", x.what());
        } catch (...) {
            std::fprintf(stderr, "alamo_headless: uncaught exception\n");
        }
        std::abort();
    });
    std::vector<char*> args;
    bool profile_on = false;
    std::string profile_out;
    double budget_ms = 1000.0 / 30.0;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--profile") profile_on = true;
        else if (a == "--profile-out" && i + 1 < argc) profile_out = argv[++i];
        else if (a == "--profile-budget" && i + 1 < argc) budget_ms = std::strtod(argv[++i], nullptr);
#ifdef ALAMO_SIM_HAVE_BATTLES
        // SP5: how a battle tick runs its services graph (results are the same in every mode; the modes are checks).
        else if (a == "--svc-mode" && i + 1 < argc) {
            const std::string m = argv[++i];
            auto d = alamo::sim::TacticalBattle::service_run_defaults();
            if (m == "serial") d.mode = alamo::tactical::services::RunMode::Serial;
            else if (m == "parallel") d.mode = alamo::tactical::services::RunMode::Parallel;
            else if (m == "reverse") d.mode = alamo::tactical::services::RunMode::Reverse;
            else {
                std::fprintf(stderr, "alamo_headless: --svc-mode serial | parallel | reverse\n");
                return 2;
            }
            alamo::sim::TacticalBattle::set_service_run_defaults(d);
        } else if (a == "--svc-streams" && i + 1 < argc) {  // SP5 t03: services on counter-based streams (moves draws)
            auto d = alamo::sim::TacticalBattle::service_run_defaults();
            d.streams = 0;
            const std::string list = argv[++i];
            size_t at = 0;
            while (at <= list.size()) {
                size_t end = list.find(',', at);
                if (end == std::string::npos) end = list.size();
                const std::string n = list.substr(at, end - at);
                if (n == "hangar") d.streams |= 1u << static_cast<uint32_t>(alamo::tactical::services::StreamId::Hangar);
                else if (!n.empty() && n != "none") {
                    std::fprintf(stderr, "alamo_headless: --svc-streams: unknown service '%s' (hangar | none)\n", n.c_str());
                    return 2;
                }
                at = end + 1;
            }
            alamo::sim::TacticalBattle::set_service_run_defaults(d);
        } else if (a == "--svc-parallel-min" && i + 1 < argc) {  // SP5: per-unit service loops fork from this many units
            auto d = alamo::sim::TacticalBattle::service_run_defaults();
            d.parallel_min = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            alamo::sim::TacticalBattle::set_service_run_defaults(d);
        } else if (a == "--svc-check") {
            auto d = alamo::sim::TacticalBattle::service_run_defaults();
            d.check = true;
            alamo::sim::TacticalBattle::set_service_run_defaults(d);
        }
#endif
        else args.push_back(argv[i]);
    }
    if (profile_on || !profile_out.empty()) alamo::profile::start(65536);
    const int rc = headless_main(static_cast<int>(args.size()), args.data());
    if (alamo::profile::enabled()) {
        alamo::profile::stop();
        const std::string text = alamo::profile::format_report(alamo::profile::build_report(alamo::profile::snapshot(), budget_ms));
        std::fputs(text.c_str(), stdout);
        if (!profile_out.empty()) {
            if (FILE* f = std::fopen(profile_out.c_str(), "wb")) {
                std::fputs(text.c_str(), f);
                std::fclose(f);
            } else {
                std::fprintf(stderr, "cannot write %s\n", profile_out.c_str());
            }
        }
    }
    return rc;
}
