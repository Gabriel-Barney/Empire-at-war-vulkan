// alamo_headless --map: a tactical map through the G0 host (engine/apps/sim).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct MapRunOptions {
    std::string map;           // e.g. _MP_SPACE_KUAT
    std::string game_dir;      // FoC "corruption" folder (default: ALAMO_FOC_DATA)
    std::vector<std::string> game_args;  // MODPATH= ...
    uint64_t ticks = 600;
    bool ticks_set = false;
    int32_t workers = -1;
    uint32_t seed = 1;
    std::string record_orders;  // write the player order stream (.acmd)
    std::string replay;         // replay a recorded stream (.acmd); seed + ticks from the stream
    std::string crc_out, golden;
    bool check_workers = false;
    bool realtime = false;      // pace at the speed table rate (30 ticks/s) instead of as fast as possible
    bool ai = true;             // AI stand-in controllers (synced RNG, execute_direct)
    bool quiet = false;
    bool expect_battle_end = false;  // --skirmish-ai: fail unless the battle ends with a winner before the frame limit
    bool expect_skirmish_base = false;  // --skirmish-ai foc-land (t50): fail unless both sides start with their pre-built base and fight
    std::string human;          // --skirmish-ai foc-land / skirmish (skai): the faction whose slot is an idle human
    // OPT2 t01 / t02 parity knobs (--galactic-ai, --skirmish-ai): --ai-latency F (AI path recompute latency, default
    // galactic::kDefaultAllPairsLatency; 0 = the original timing) and --no-ai-stagger (every AI player plans on the same
    // frames, as the original). Not recorded in saves.
    int64_t ai_latency = -1;  // < 0: the default
    bool ai_stagger = true;
};

int map_run(const MapRunOptions& opt);
// --economy: a galactic campaign (opt.map = campaign name, empty = first) through the G0 host with the
// S3 economy (GalacticSim) as the galactic mode tick; game data from opt.game_dir (VFS) or data_dir.
int economy_run(const MapRunOptions& opt, const std::string& data_dir);
// --story (S4): --economy plus the campaign's story plots and Lua scripts (SimHost::start_story). Writes
// the story log (triggers + fired events, StorySession::log_text) to story_out and per-tick CRCs (with
// the "story" system) to opt.crc_out; story_golden / opt.golden compare them. --check-workers runs
// 0/1/4/N workers and requires identical logs and CRCs.
int story_run(const MapRunOptions& opt, const std::string& data_dir, const std::string& story_out,
              const std::string& story_golden);
