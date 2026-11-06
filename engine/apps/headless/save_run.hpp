// alamo_headless N1 save / load (owner: n1-host): saving a running session, loading it into a fresh one, and the N1
// done-check (docs: engine/apps/sim/include/alamo/sim/save_session.hpp).
//
//   --save-session synthetic|hotm|galactic [--campaign NAME] --save-at T FILE [--ticks K] [--crc-out F]
//       Builds the session, runs to tick T (or the first later tick where saving is allowed), saves to FILE, then
//       runs K more ticks (default 0). --crc-out writes the whole run's per-tick CRC log.
//   --load FILE [--ticks K] [--crc-out F] [--replay STREAM.acmd] [--record-orders OUT.acmd]
//       Rebuilds the session the save describes (its "session" chunk: kind, campaign, seed, players, options, data
//       fingerprint), loads every chunk and runs K ticks. --crc-out writes the CRC log from the save tick on (its
//       first record is the loaded state). --replay applies a recorded command stream on top of the save
//       (CommandPipeline::replay_from_save: a stream recorded after loading this save, or a full tick-0 stream of the
//       same run); --record-orders records the orders given after the load as a stream continuing the save.
//   --save-session KIND --save-check --save-at T FILE --ticks K [--expect-pending a,b,...]
//       The N1 done-check: run to T, save, continue to T+K (CRC log A); load the save into a fresh session in this
//       process and run to T+K (log B); run `--load FILE` in a fresh process (log C); B and C must equal A's records
//       from T on (first_divergence names the first diverging tick + system). Also: a session with another seed
//       must refuse the save. Chunks that do not load yet because their system's write/read has not landed are
//       "pending": when every failing chunk is listed in --expect-pending the check verifies the CRC systems whose
//       chunks did load at the save tick, prints "PENDING: <chunks>" and exits 77 (ctest SKIP_RETURN_CODE); a failing
//       chunk not in the list fails the check (exit 1). Listed chunks that now load are reported so the list can be
//       emptied.
//
// Sessions:
//   synthetic  no game data: a 5-planet galaxy (Empire / Underworld / Neutral), a story plot set with a Lua script
//              (when built with engine/lua), the Empire buying units through the scheduled command pipeline and the
//              Underworld moving / invading through execute_direct (state-driven stand-ins, nothing outside the sim).
//              synthetic-nostory: the same without the story (no Lua).
//   hotm       FoC Sandbox_Heart_Of_The_Maw_Underworld as alamo_headless --economy --galactic-ai runs it: every
//              playable faction a galactic AI player, conflicts auto-resolved (CampaignBattles). Needs ALAMO_FOC_DATA
//              (or --game); skipped (exit 0) without it.
//   galactic   the same for --campaign NAME.
//   skirmish   (N1 stage 2) the --skirmish-ai battle (skirmish_run.hpp) of --campaign synthetic | foc | foc-land (default
//              foc; --map overrides the map): a 3-planet galaxy whose attacker fleet arrives in the defender's orbit,
//              fought AI vs AI with the tactical AI. foc / foc-land need ALAMO_FOC_DATA (skipped without it).
//
// Stage 2 options (n2-host; recorded in the save's descriptor so --load rebuilds the same session):
//   --battles auto|fight|fight-all[+anim]   hotm / galactic: how conflicts are resolved (default auto); fight-all
//              fights AI-vs-AI conflicts tactically with the tactical AI on every AI side. "+anim": the sim-owned
//              animation (SimAnimation, "anim" chunk / CRC system); skirmish: only "+anim" counts.
//   --story    hotm / galactic: the campaign's story plot sets (StorySession, story CRC).
//   --save-when any|battle|galactic|pending|scheduled   save at the first tick >= T where saving is allowed and: any; a
//              tactical battle is on the mode stack; no battle on the stack (the battles then start after the load); a
//              conflict is queued / being decided with no battle running (not reachable in the all-AI sessions: the conflict
//              manager decides and the battle host pushes the battle within one tick); scheduled (OPT3 t04): an AI background
//              job (galactic or land path recompute, latency_job.hpp) is in flight.
// The check also asserts that the synced RNG (state + draw count) after loading equals the saving run's at the save
// tick and at T+K.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SaveRunOptions {
    std::string session;        // --save-session: synthetic | hotm | galactic
    std::string campaign;       // --campaign (galactic)
    std::string save_file;      // --save-at T FILE / check
    uint64_t save_at = 0;
    bool save_at_set = false;
    std::string load_file;      // --load FILE
    bool check = false;         // --save-check
    uint64_t ticks = 0;         // K
    std::string crc_out;
    std::string replay;         // --replay with --load
    std::string record_orders;  // --record-orders with --load
    std::vector<std::string> expect_pending;
    std::string game_dir;
    std::vector<std::string> game_args;
    int32_t workers = -1;
    uint32_t seed = 7;
    bool seed_set = false;
    bool quiet = false;
    std::string self;           // argv[0]: the fresh-process half of the check
    // N1 stage 2 (n2-host)
    std::string battles;            // --battles auto|fight|fight-all[+anim]
    bool story = false;             // --story
    std::string map;                // --map (skirmish)
    std::string save_when = "any";  // --save-when any|battle|galactic|pending|scheduled
};

int save_run(const SaveRunOptions& opt);

// --- BM1 benchmark (bench_run.cpp): sessions loaded from a save, any number of times, and a save made for a scenario ---
namespace alamo {
struct CrcLog;
namespace commands {
struct CommandRecording;
}
}  // namespace alamo

// One session loaded from a save, ready to tick.
class LoadedSession {
public:
    virtual ~LoadedSession() = default;
    virtual bool tick(std::string& err) = 0;  // one SimHost tick + the harness step after it (as save_run's runs)
    virtual const alamo::CrcLog& crc_log() const = 0;  // from the save tick on (first record: the loaded state)
    virtual uint64_t frame() const = 0;
    virtual size_t replay_failures() const = 0;
    virtual uint32_t worker_count() const = 0;  // the job system's workers (after resolving "machine default")
};

// A save read once, with the game data its session needs loaded once; load() builds a fresh session and loads the save
// into it (the harness of --load FILE).
class SaveSessionSource {
public:
    virtual ~SaveSessionSource() = default;
    virtual uint64_t save_tick() const = 0;
    virtual std::string describe() const = 0;  // "<kind> '<campaign>' seed <s>"
    // `replay` (may be null): a stream continuing the save, applied with replay_after_load. `record` (may be null): records
    // the orders given after the load as a stream continuing the save.
    virtual std::unique_ptr<LoadedSession> load(int32_t workers, const alamo::commands::CommandRecording* replay,
                                                alamo::commands::CommandRecording* record, std::string& err) = 0;
};
// 1 = opened; 0 = the session needs data that is not there (skipped, err says why); -1 = error.
int open_save_source(const std::string& save_path, const std::string& game_dir, const std::vector<std::string>& game_args,
                     std::unique_ptr<SaveSessionSource>& out, std::string& err);
// Builds a --save-session opt.session session (opt.campaign / map / battles / story / seed / workers), runs it to the
// save point (opt.save_at, opt.save_when) and writes opt.save_file. 1 = written (*saved_tick set); 0 = skipped (no data);
// -1 = error.
int make_save(const SaveRunOptions& opt, uint64_t& saved_tick, std::string& err);
