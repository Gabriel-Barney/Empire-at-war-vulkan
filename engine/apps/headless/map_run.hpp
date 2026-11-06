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
    // q6 t02: --skirmish-ai foc-land / skirmish --ai FACTION[,FACTION]: the slots' factions (empty = Empire, Rebel). Two names:
    // slot 0, slot 1 (--human may name one of them); one name: the AI slot (slot 1) beside the --human faction (slot 0).
    std::string ai_factions;
    bool fail_on_lua_errors = false;  // --skirmish-ai: fail on any tactical AI Lua error (foc-station always does), a1j t07
    // OPT2 t01 / t02 parity knobs (--galactic-ai, --skirmish-ai): --ai-latency F (AI path recompute latency, default
    // galactic::kDefaultAllPairsLatency; 0 = the original timing) and --no-ai-stagger (every AI player plans on the same
    // frames, as the original). Not recorded in saves.
    int64_t ai_latency = -1;  // < 0: the default
    bool ai_stagger = true;
    // OPT2 t10 (--skirmish-ai): --no-pp-streams (slot-10 plan picks and solves draw from the synced stream in proposal order,
    // the original) and --pp-latency F (frames from a slice to the apply of its solves, default
    // ai::tactical::kDefaultPpLatency). Not recorded in saves.
    bool pp_streams = true;
    int64_t pp_latency = -1;  // < 0: the default
    // q6 t06 (--story, --galactic-ai): --scoring runs the GameConstants Game_Scoring_Script_Name script at the session start
    // (SimHost::set_scoring_script) as the game does. Off by default: its GlobalValue writes enter the frame CRC.
    bool scoring = false;
    // q6 (--skirmish-ai): --object-scripts turns on SimConfig::object_scripts (game-object Lua scripts, OBJECT_SCRIPTS.md)
    // and the GameScoring script (mods set GlobalValue MOD_ID there, which their object scripts read). Off by default.
    bool object_scripts = false;
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
