// S5 / G0 game host: the main loop around the mode stack (docs/re/MAIN_LOOP.md Â§1, Â§6), the command
// pipeline (engine/commands), synced randomness and the per-tick CRC (Q1).
//
// ONE TICK (tick_once), in the original's order (MAIN_LOOP.md Â§1.2 T1..T14):
//   T1  Begin_Tick: global frame += 1
//   T2  scheduled commands with frame <= new frame: CommandPipeline::begin_tick(frame) (drain in
//       tick -> player -> sequence order, batch CRC hook, recording/replay, handler per command)
//   T4  TreeCull: rebuild dirty spatial trees of the top tactical mode (sim time = frame / 30)
//   T5  GameModeManager::Update: the GameScoring Service (q6 t09, set_scoring_script), then ONLY the top mode's tick
//   T6  scene animations [stub]
//   T7  PlayerList update: per player in index order, the player's controller (AI / script stand-ins
//       issue orders through execute_direct here, like the original AI)
//   T8  global Lua [stub]
//   T9  story (S4): StorySession::service when start_story ran (story_session.hpp); trace step
//       "T9.story" only then; then the GameScoring galactic event calls (q6 t09; "T9.scoring" only with a script)
//   T10-T11 tutorial, campaign end [stubs]
//   T12 Post_Tick: top mode frame += 1 unless paused
//   T13 End_Tick: event bus delivery, then the CRC record of this frame (CrcRecorder)
// The headless "as fast as possible" runner (run_fast) and the real-time loop (loop_iteration /
// run_realtime: FrameSynchronizer pacing, at most one tick per iteration, no catch-up, FPS-cap
// sleep) call the same tick_once, so both produce the identical tick sequence and CRCs.
//
// Session (Start_Game on an empty stack): frame = 0, FrameSynchronizer reset, synced RNG seeded
// from the host seed (the host supplies it: SP = QPC in the original, MP = host's seed, replays =
// recorded seed), id counter reset. Pushing a tactical mode on top does NOT reseed.
//
// COMMANDS. Wire ids are 32-bit object ids (commands::kNoObject = 0x3FFFFF = none) resolved in the
// top mode's primary store at execution time (wire_id / resolve). Handled events: Select,
// SelectAll, MoveToPosition, MoveToObject, Attack, StopMovement (others -> Unhandled). Rules
// (COMMANDS.md Â§6.2): resolve at execution, missing / dying / foreign units are silently skipped,
// orders act on the issuer's current selection (synced per-player state) when the event says so,
// results never depend on the local player. AI / Lua call execute_direct (no queue, no recording).
//
// CRC systems (registration order, part of the golden files):
//   frame     global frame + mode stack (kind, mode frames, counters, orders)
//   objects   every mode's primary store, bottom -> top (ObjectStore::hash_state)
//   galactic.state  S3 data packs (GalacticSim::hash_state = register_crc(reg, "galactic", false);
//             registered once by the host and forwarded to the current session's sim, if any)
//   spatial   top tactical mode's TreeSet::hash_state (0 entries when none)
//   players   per-player selections
//   rng       synced RNG (state + draws)
//   commands  digest of this tick's canonical drained batch bytes
//   story     only with SimConfig::story_crc (S4): StorySession::hash (false when no session), so
//             existing goldens keep their system list
// Parity parts: objects.parity (top mode primary store: sum of per-object CRCs), rng.parity
// (crc32_raw of the synced state); PLR term not modelled (0). Secondary stores are never hashed.
//
// N1 SAVE / LOAD (save_session.hpp has the chunk list and the load procedure): save() writes the session descriptor
// (set_session) and one chunk per system; load() is called on a session rebuilt like a new game of the saved
// descriptor, before its first tick, and reads every chunk over it. After a successful load the CRC log restarts:
// the next tick_once first records the loaded state at the saved frame (its CRC equals the saving run's record of
// that frame), then continues. Stage 2: also during a tactical battle (the rebuild procedure in save_session.hpp).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "alamo/core/profile.hpp"
#include <vector>

#include "alamo/commands/pipeline.hpp"
#include "alamo/commands/recording.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/jobs/tick.hpp"
#include "alamo/sim/game_modes.hpp"
#include "alamo/sim/object_scripts.hpp"
#include "alamo/sim/pacing.hpp"
#include "alamo/sim/save_session.hpp"
#include "alamo/sim/story_session.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/lua_vm.hpp"           // q6 t06: the GameScoring script VM
#include "alamo/lua/script_remaining.hpp"  // Int-final: GlobalValueStore
#endif

namespace alamo {
class FileProvider;
}
namespace alamo::lua {
class GlobalValueStore;
class LuaVm;
class LuaPlayerWorld;
struct AiEventArg;
}  // namespace alamo::lua
namespace alamo::galactic {
struct GalacticRules;
struct GalacticSetup;
}  // namespace alamo::galactic

namespace alamo::sim {

// Q6: SimConfig::players for a galactic session over a setup with `setup_players` players: at least 8 (the default, so
// stock FoC sessions keep their CRC), at most kMaxPlayers (start_galactic refuses a setup with more players than this).
inline uint32_t session_players(size_t setup_players) {
    return static_cast<uint32_t>(setup_players < 8 ? 8 : setup_players > kMaxPlayers ? kMaxPlayers : setup_players);
}

struct SimConfig {
    uint32_t seed = 0;                                          // synced RNG session seed
    uint32_t free_seed = 0x12345678u;                           // free RNG (never sim state)
    uint32_t players = 8;                                       // PlayerList size (<= kMaxPlayers); see session_players
    commands::SchedulerConfig scheduler = commands::SchedulerConfig::single_player();
    bool story_crc = false;                                     // register the "story" CRC system (S4)
    // q6: game-object Lua scripts (object_scripts.hpp, docs/re/OBJECT_SCRIPTS.md). Off by default: the attach and the
    // first-pump draws change the synced stream, so sessions without it keep their CRC streams and goldens.
    bool object_scripts = false;
};

// Per-player controller run at T7 (AI / script stand-ins). May call execute_direct.
class SimHost;
using PlayerController = std::function<void(SimHost& host, uint8_t player, uint64_t frame)>;

class SimHost {
public:
    SimHost(const objects::GameObjectTypes& types, jobs::JobSystem& js, SimConfig cfg = {});
    SimHost(const SimHost&) = delete;
    SimHost& operator=(const SimHost&) = delete;

    // ---- session / mode stack ----
    GameMode& start_game(ModeKind kind);  // Start_Game (push)
    bool end_game_mode();                 // End_Game_Mode (pop)
    // Galactic campaign with the S3 economy: Start_Game(galactic) (new session if the stack is empty),
    // then GalacticSim::start on the mode's primary store with the SYNCED RNG. Returns nullptr (and
    // pops the mode) when the setup is unusable, or has more players than SimConfig::players (Q6: those players would
    // get no controller and no orders; size the host with session_players). Galactic events (MoveObjectToObject,
    // MoveThroughObjects, Production, Invade) then go to GalacticSim::handle_command.
    galactic::GalacticSim* start_galactic(const galactic::GalacticRules& rules, const galactic::GalacticSetup& setup,
                                          std::vector<std::string>* diagnostics = nullptr);
    GalacticMode* galactic_mode();  // the galactic mode in the stack (bottom), or nullptr
    // S4: story plots + scripts for the running galactic campaign (after start_galactic). Turns on the
    // GalacticSim event stream. False (no session) without a galactic sim, when engine/lua is not built,
    // or when a plots file cannot be loaded. The session ends with the galactic mode / game session.
    bool start_story(const StoryStart& start, std::vector<std::string>* diagnostics = nullptr);
    StorySession* story() { return story_.get(); }
    const StorySession* story() const { return story_.get(); }
    static bool story_supported();  // built with engine/lua
    // Int-final: the session-wide GlobalValue table (L1_S4_REMAINING §1.5: one map shared by every script and mode,
    // cleared for a new game). Attached to the story VMs (StorySession), the tactical AI VMs (BattleAi) and, by the
    // hosts, the galactic AI (GalacticAiWorld::set_global_values). Hashed at the end of the "frame" CRC section only
    // while it is non-empty (sessions that never set a GlobalValue keep their CRC stream). Null without engine/lua.
    lua::GlobalValueStore* global_values();
    // q6 t06 (docs/re/GAME_SCORING.md): the GameScoring script (GameConstants Game_Scoring_Script_Name), re-created at
    // every new session in start_game (empty stack): after the GlobalValue clear, before start_galactic sets the
    // players up and before start_story loads the plots, a fresh VM runs the file's top level and Base_Definitions()
    // and gets its main thread. Mods set session globals there (Thrawn's Revenge: GlobalValue MOD_ID, which its plot
    // scripts read; vanilla: Allow_AI_Controlled_Fog_Reveal = 1, read by the BurnUnits AI plans). Dropped when the
    // stack empties [port: the original keeps it until the next session]. Empty name (default) = no script.
    //
    // q6 t09, the Service (§2) and the event calls (§3). Every call into the script runs on the sim thread at a fixed
    // point of the tick, in an order that depends only on sim state (never on the worker count), and the VM draws
    // GameRandom from the session RNG like every script (GameRandom.Free_Random keeps the free stream, LuaVm):
    //   * T5, GameModeManager::update's "gmm.scoring" step (before the mode tick): first the queued
    //     Game_Mode_Starting_Event(mode, map) of the modes set up since the last frame (galactic: start_galactic, map =
    //     set_scoring_campaign() else the SessionDescriptor campaign; tactical: scoring_mode_started from the battle host
    //     [port: delivered at the next frame's service instead of inside the setup]); then the Service: no number
    //     ServiceRate = nothing; now = (float)frame / (float)30 as a double; when LastService is not a number or
    //     now - LastService > ServiceRate: the script's threads are resumed once (LuaVm::service_threads, time = now,
    //     frame = global frame), then the script is RELEASED (VM dropped, no more calls this session) when it called
    //     ScriptExit or any call into it (event or thread) raised a Lua error since the last Service, else LastService =
    //     now. A release found pending at a due Service skips the pump.
    //   * end_game_mode: Game_Mode_Ending_Event(mode) for every popped mode, before the pop.
    //   * after T9: the galactic events the story session drained this tick (StorySession::set_scoring_tap; only with a
    //     story session), in stream order: PlanetOwnerChanged -> Galactic_Planet_Faction_Change(planet, new, old) when
    //     new != old (a missing player = the neutral player); ProductionBegin / Canceled -> Galactic_Production_Begin_ /
    //     _Canceled_Event(planet, type); ProductionCompleted -> Galactic_Production_End_Event(planet, object (its team
    //     container), else the type); ObjectDestroyed -> Galactic_Unit_Destroyed_Event(object, neutral player) only while
    //     the object is still alive in the store [port: the stream is read after the destroy, so in practice skipped].
    //   * scoring_reset_tactical_stats(): Reset_Tactical_Stats() (the battle host at auto-resolve begin).
    // Missing functions are skipped; calls are protected and not threads (they cannot yield). With a story session the
    // scoring VM also gets the story's player world, object bridge and presentation families.
    void set_scoring_script(const FileProvider* files, std::string name);
    const lua::LuaVm* scoring_vm() const;
    // "<frame> GameScoring: <what>" lines: load / Base_Definitions errors and the script's own messages.
    const std::vector<std::string>& scoring_messages() const { return scoring_messages_; }
    // q6 t09: map_name of the galactic Game_Mode_Starting_Event (the campaign name); empty = SessionDescriptor::campaign.
    void set_scoring_campaign(std::string name) { scoring_campaign_ = std::move(name); }
    // q6 t09: the battle host, once a tactical mode is set up (map loaded): queues Game_Mode_Starting_Event.
    void scoring_mode_started(ModeKind kind, std::string map_name);
    // q6 t09: Reset_Tactical_Stats() at auto-resolve begin (BATTLE_TRANSITIONS §"auto-resolve").
    void scoring_reset_tactical_stats();
    // ui1 t05 (GAME_SCORING.md §7, 0x485760): the statistics dialog's value for `control_id`
    // (IDC_MILITARY_EFFICIENCY_STATIC, IDC_CONQUEST_EFFICIENCY_STATIC, IDC_KILL_EFFICIENCY_STATIC, IDC_TITLE_STATIC, ...):
    // Get_Game_Stat_For_Control_ID(PlayerWrapper, control_id, tactical) of the scoring script, its first result as a
    // number or a string. False without a script / function or when the call fails. UI thread between ticks (the
    // script's globals it writes are not hashed).
    bool scoring_game_stat(int32_t player, std::string_view control_id, bool tactical, double& number, std::string& text,
                           bool& is_number);
    // q6 t09 diagnostics: Services that pumped the threads this session, event calls made, releases (exit / error).
    uint64_t scoring_services() const { return scoring_services_; }
    uint64_t scoring_calls() const { return scoring_calls_; }
    uint64_t scoring_releases() const { return scoring_releases_; }
    // q6: game-object Lua scripts (SimConfig::object_scripts; object_scripts.hpp). Null when off. The host hooks every
    // pushed mode's primary store, services the scripts at T5 after the store update, attaches the session GlobalValue
    // table, the galactic player world (start_galactic; the story's in start_story) and the story's object bridge;
    // battle hosts set their store's bridge / world (ObjectScriptHost::set_bridge). Scripts load through
    // set_object_script_files (null: nothing attaches).
    ObjectScriptHost* object_scripts() { return object_scripts_.get(); }
    const ObjectScriptHost* object_scripts() const { return object_scripts_.get(); }
    void set_object_script_files(const FileProvider* files);
    // The player world object scripts of a tactical store without a story session use (the galactic sim's players);
    // null before start_galactic, without object scripts or without the players family.
    lua::LuaPlayerWorld* object_script_players() { return object_players_.get(); }
    GameModeManager& modes() { return modes_; }
    const GameModeManager& modes() const { return modes_; }
    TacticalMode* tactical() { return modes_.top_tactical(); }

    // ---- loop ----
    uint64_t frame() const { return frame_; }  // global frame (FS +0x00)
    double sim_time() const { return static_cast<double>(frame_) / static_cast<double>(kSimTicksPerSecond); }
    // One logic tick (T1..T14) + its CRC record. False (nothing done) when the stack is empty.
    bool tick_once();
    // Headless: `ticks` ticks back to back. Returns ticks run.
    uint64_t run_fast(uint64_t ticks);
    // One main-loop iteration at wall time now_ms: pacing test, at most one tick, then the
    // per-frame update with d = 0.03 per tick done (MAIN_LOOP.md Â§1.4 R1). True if it ticked.
    bool loop_iteration(uint32_t now_ms);
    // Real-time loop until `ticks` ticks ran: now_ms() is the wall clock (timeGetTime-style ms),
    // sleep_ms(n) waits; the FPS cap (floor(1000/cap) - 1 ms per iteration) paces iterations.
    // after_tick (optional) runs right after every tick (input between ticks).
    uint64_t run_realtime(uint64_t ticks, const std::function<uint32_t()>& now_ms,
                          const std::function<void(uint32_t)>& sleep_ms, uint32_t fps_cap = kDefaultFpsCap,
                          const std::function<void()>& after_tick = {});
    FrameSynchronizer& pacing() { return pacing_; }

    // ---- commands ----
    commands::CommandPipeline& pipeline() { return *pipeline_; }
    // Local player orders (scheduled; SP delay 1).
    commands::SubmitResult submit(uint8_t player, commands::EventData ev);
    // AI / Lua direct path: runs now, inside the caller's tick slot.
    commands::RejectReason execute_direct(uint8_t player, commands::CommandSource source,
                                          const commands::EventData& ev);
    // Records drained batches into rec (nullptr stops). Call before the first tick.
    void record_to(commands::CommandRecording* rec);
    // Replays rec (rebuilds the pipeline with rec->config). Call before the first tick.
    void replay_from(const commands::CommandRecording* rec);
    uint64_t commands_applied() const { return applied_; }
    uint64_t commands_rejected() const { return rejected_; }

    // Wire id <-> handle in the top mode's primary store.
    uint32_t wire_id(ObjectHandle h) const;
    ObjectHandle resolve(uint32_t wire_id) const;  // alive objects only; null otherwise
    const std::vector<ObjectId>& selection(uint8_t player) const { return selection_.at(player); }

    // ---- players ----
    uint32_t players() const { return cfg_.players; }
    uint32_t session_seed() const { return cfg_.seed; }  // A2 tactical: the perception '#' reseed (AI_GALACTIC_3 §7.3)
    void set_controller(uint8_t player, PlayerController c) { controllers_.at(player) = std::move(c); }

    // ---- randomness / CRC ----
    GameRandom& random() { return random_; }
    CrcRegistry& crc_registry() { return registry_; }
    const CrcLog& crc_log() const { return recorder_->log(); }
    CrcLog& crc_log() { return recorder_->log(); }
    // SIMP sp2 (docs/SIM_PARALLEL_DESIGN.md 4.5): CRC capture cadence. every == 1 (the default: headless, tests,
    // --crc-out) records every tick. every > 1 records the initial state, the ticks with tick % every == 0 and on-demand
    // captures (capture_crc_now: on save, at a benchmark's end, before a compare), and the log is a checkpoint log
    // (CrcLog::checkpoint = every, so first_divergence compares stored ticks). ring > 0 keeps at most about `ring` records
    // (the oldest are dropped). Only which ticks are hashed changes, never the sim. set_default_crc_cadence is the
    // process-wide default a new host starts with (alamo_game's main); set_crc_cadence changes this host.
    static void set_default_crc_cadence(uint32_t every, size_t ring = 0);
    void set_crc_cadence(uint32_t every, size_t ring = 0);
    uint32_t crc_cadence() const { return crc_every_; }
    void capture_crc_now();  // records the current frame unless the log's last record already is it
    jobs::JobSystem& jobs() { return *js_; }
    const ParallelFor& parallel_for() const { return pf_; }

    // Optional step trace (tests): names of the tick steps in execution order.
    void set_trace(std::vector<std::string>* t) { trace_ = t; }

    // ---- S10 battle transitions hook (campaign_battles.hpp) ----
    // Runs right after T5 (GameModeManager::Update) on every tick: pushes / ticks / pops the tactical battle
    // of a galactic conflict. Trace step "T5.battles" only while a hook is set.
    using ModeUpdateHook = std::function<void(SimHost& host)>;
    void set_after_mode_update(ModeUpdateHook hook) { after_mode_update_ = std::move(hook); }
    // ---- S10-int hooks (battle integration) ----
    // T5 object-animation slot (sim_anim.hpp: model creation + AnimWorld advance), right after the battle
    // hook. Trace step "T5.anim" only while set.
    void set_object_animation_hook(ModeUpdateHook hook) { object_anim_ = std::move(hook); }
    // Tactical commands this host does not handle itself (ability commands 40 / 43 / 57 / 58 -> the running
    // battle): called from apply() at the command phase while a tactical mode is on top. Unset: Unhandled.
    using TacticalCommandHook = std::function<commands::RejectReason(SimHost& host, uint8_t player, const commands::EventData& ev)>;
    void set_tactical_command_hook(TacticalCommandHook hook) { tactical_cmd_ = std::move(hook); }
    // Int-final: galactic commands GalacticSim::handle_command leaves Unhandled (SpecialAbility 13, e.g. the FoC
    // corruption choice -> CampaignBattles::start_corruption), called at the command phase while the galaxy is on top.
    void set_galactic_command_hook(TacticalCommandHook hook) { galactic_cmd_ = std::move(hook); }
    // --- P host (owner: game host agent): human unit orders -> the running battle ---
    // Called from apply() at the command phase for MoveToPosition / MoveToPositionFacing / MoveToRay / MoveToRayFacing / Facing /
    // MoveToObject / Attack / Escort (guard) / StopMovement / Retreat / Reinforce while a tactical mode is on top, BEFORE the G0 stub-order path: a game host routes them into
    // the battle's MovementSystem / CombatSystem (apps/game). `selection` = the issuer's synced selection. Unhandled
    // falls through to the stub path; any other result is the command's result. Orders still come only from the
    // drained command batch, so recordings / lockstep stay valid as long as the replaying host installs the same hook.
    using OrderHook = std::function<commands::RejectReason(SimHost& host, uint8_t player, const commands::EventData& ev,
                                                           const std::vector<ObjectId>& selection)>;
    void set_order_hook(OrderHook hook) { order_hook_ = std::move(hook); }
    // q6 (COMMANDS §2.4.1): Select All's tests that need the running battle (CampaignBattles): false = skip the owned
    // SELECTABLE object `h` (a TEAM member, a garrisoned unit, a running retreat of its owner, Set_Selectable(false)); a
    // TEAM container appends the members that join it. Unset: only the store tests.
    using SelectAllHook = std::function<bool(SimHost& host, uint8_t player, ObjectHandle h, std::vector<ObjectId>& members)>;
    void set_select_all_hook(SelectAllHook hook) { select_all_hook_ = std::move(hook); }
    // Port-only events (PortExtension 63, e.g. the pause / resume entry of docs/DECISIONS.md "Camera and controls") offered to
    // the game host at the command phase in every mode, before the galactic / tactical handlers; Unhandled falls through.
    // P4 host: also offered every other event that no handler took (Unhandled after the galactic / tactical paths, e.g.
    // the auto-resolve dialog's Withdrawl 49); the hook's answer is then the command's result.
    using PortEventHook = std::function<commands::RejectReason(SimHost& host, uint8_t player, const commands::EventData& ev)>;
    void set_port_event_hook(PortEventHook hook) { port_hook_ = std::move(hook); }
    // Control groups (COMMANDS.md ControlGroup 30, UI.md 4.4) in tactical modes: assign = the issuer's synced selection
    // replaces group n (add: appended, ascending id); recall = the selection becomes the group's live owned members.
    // Per player, 10 groups; cleared with the selection at a new session. In the "players" CRC section once any group
    // was assigned (sessions without control groups keep their CRC stream).
    const std::vector<ObjectId>& control_group(uint8_t player, uint8_t group) const { return groups_.at(player).at(group % 10u); }
    // STATION_HANGARS_SOUND.md §1.2 step 6 (skirmish station upgrade S -> S'): per player in index order, S' joins the
    // selection when S is in it (appended) and every control group holding S (ascending id). S itself leaves with its
    // removal (the selection / groups drop dead objects as before).
    void station_replaced(ObjectId old_station, ObjectId new_station);
    // --- end P host ---
    // --- P4 host (owner: game host agent): presentation tap right before T9 on every tick, while the galactic event
    // stream still holds this tick's T2..T8 events (StorySession drains it at T9). Read-only: it must not change sim
    // state, draw synced randomness or issue orders. No trace step. ---
    void set_before_story_hook(ModeUpdateHook hook) { before_story_ = std::move(hook); }
    // --- end P4 host ---
    // --- OI host (owner: game host agent): right after T9 on every tick (also without a story session), before T12 /
    // the T13 frame CRC capture. For the frame-pacing / parity inputs that follow sim-computed Lua presentation state
    // (cinematic rate lock -> pacing() and the registry's ParityFrameOptions::invert). It must not change hashed sim
    // state, draw synced randomness or issue orders. No trace step. ---
    void set_after_story_hook(ModeUpdateHook hook) { after_story_ = std::move(hook); }
    // --- end OI host ---
    // q6: the story reward SWITCH_CONTROL (StorySession::set_switch_control_hook) -> the host's AI players. Kept here and
    // installed on every story session start_story creates, before its plots load (a load-time Story_Event can fire it).
    // Set it before start_story; the hook runs on the sim thread inside T9 (or the load) and may call set_controller.
    void set_switch_control_hook(std::function<void(const StorySwitchControl&)> hook) {
        switch_control_ = std::move(hook);
        if (story_) story_->set_switch_control_hook(switch_control_);
    }

    // ---- N1 save / load (owner: n1-host; save_session.hpp) ----
    // The application describes the session it built (kind, campaign, data, players, options, data fingerprint);
    // save() stores it as the "session" chunk with this host's SimConfig filled in, load() compares it.
    void set_session(SessionDescriptor d) { session_ = std::move(d); }
    // The descriptor as save() writes it (SimConfig fields from this host).
    SessionDescriptor session() const;
    // Systems the application attached (galactic AI, battles, animation, its own controllers' state): one chunk each,
    // after the host's chunks, in registration order. Throws std::invalid_argument for a bad / duplicate name.
    // Participants that capture objects must remove themselves before those die (remove_save_participant).
    void add_save_participant(SaveParticipant p);
    void remove_save_participant(std::string_view name);
    // True between ticks of a running session unless something cannot be saved now (save_session.hpp "WHEN SAVING IS
    // REFUSED"): a tactical mode no attached system can rebuild, a story or participant blocker. `why` gets the reason.
    bool save_allowed(std::string* why = nullptr) const;
    // Every chunk of the session (save_session.hpp list). False (+ *err) when !save_allowed().
    bool save(SaveFile& out, std::string* err = nullptr) const;
    // Call on a freshly rebuilt session (same descriptor, before its first tick). Checks the descriptor, then reads
    // every chunk (all are tried, so `report` names every failing one). A save with more modes than the session (a
    // battle was running) is loaded in two passes: the bottom modes' state, then the participants' rebuild callbacks
    // push the saved upper modes like a new battle, then every chunk is read (save_session.hpp "LOADING A SAVE TAKEN
    // DURING A BATTLE"). False: *err names the failing chunks and the session must be discarded (systems whose chunk
    // did read are loaded, the others keep their new-game state).
    bool load(const SaveFile& in, std::string* err = nullptr, LoadReport* report = nullptr);
    // Replays / records on top of a loaded session (commands/recording.hpp "REPLAY FROM A SAVE"): the loaded pipeline is
    // kept (record_to / replay_from would rebuild it). `at` = SavePoint{save.tick, save_digest(save.encode())}. Call after
    // load(), before the first tick. replay_after_load: false (+ *err, nothing changed) when `rec` does not continue the
    // loaded state; the seed is the save's, rec->seed is ignored. record_after_load: nullptr stops recording.
    bool replay_after_load(const commands::CommandRecording* rec, const commands::SavePoint& at, std::string* err = nullptr);
    void record_after_load(commands::CommandRecording* rec, const commands::SavePoint& at);
    // ---- end N1 ----

private:
    void make_pipeline(const commands::SchedulerConfig& cfg);
    commands::RejectReason handle(uint64_t tick, const commands::Command& cmd);
    commands::RejectReason apply(uint8_t player, const commands::EventData& ev);
    std::vector<ObjectHandle> owned_units(uint8_t player, const std::vector<uint32_t>& ids) const;
    void register_crc();
    // --- S4 (owner: S4 agent): accepted human move / guard orders of the order hook -> STORY_COMMAND_UNIT /
    // STORY_GUARD_UNIT (SCRIPT_GAPS.md §4.3 / §4.4): per unit of the order (selection order) owned by the issuer, when
    // the issuer is the tactical plot set's player. MoveToPosition / MoveToPositionFacing / MoveToRay / MoveToRayFacing -> UnitCommanded
    // with the destination (ray events: the plane point b); Escort (the guard order) -> UnitGuarding with the guarded object. MoveToObject / Attack do not
    // dispatch (only the four move-order executors and the guard order do). ---
    void story_order_feed(uint8_t player, const commands::EventData& ev, const std::vector<ObjectId>& selection);
    void step(const char* name) {
        profile::lap(name);  // OPT1: time of the phase that just finished (wall clock; never read by the sim)
        if (trace_) trace_->emplace_back(name);
    }

    jobs::JobSystem* js_;
    SimConfig cfg_;
    ParallelFor pf_;
    GameModeManager modes_;
    std::unique_ptr<StorySession> story_;  // after modes_: destroyed first (its VMs reference the stores)
    FrameSynchronizer pacing_;
    GameRandom random_;
    uint64_t frame_ = 0;
    bool session_used_ = false;
    std::unique_ptr<commands::CommandPipeline> pipeline_;
    commands::CommandRecording* record_ = nullptr;
    uint64_t batch_crc_ = 0;
    uint64_t applied_ = 0, rejected_ = 0;
    std::array<std::vector<ObjectId>, kMaxPlayers> selection_;
    std::array<PlayerController, kMaxPlayers> controllers_;
    CrcRegistry registry_;
    std::unique_ptr<jobs::CrcRecorder> recorder_;
    // cu9 t13: a fresh recorder (CRC hashers on the job system; the old parallel_crc switch was never off).
    void make_recorder() {
        recorder_ = std::make_unique<jobs::CrcRecorder>(*js_, registry_, true);
        recorder_->log().checkpoint = crc_every_ > 1 ? crc_every_ : 0;
    }
    void record_crc();  // sp2: capture this frame, then trim to the ring
    uint32_t crc_every_ = 1;  // sp2: CRC capture cadence (set_crc_cadence)
    size_t crc_ring_ = 0;
    std::vector<std::string>* trace_ = nullptr;
    ModeUpdateHook after_mode_update_;  // S10
    ModeUpdateHook object_anim_;        // S10-int
    TacticalCommandHook tactical_cmd_;  // S10-int
    TacticalCommandHook galactic_cmd_;  // Int-final
    OrderHook order_hook_;              // P host
    SelectAllHook select_all_hook_;     // q6
    PortEventHook port_hook_;           // P host
    std::array<std::array<std::vector<ObjectId>, 10>, kMaxPlayers> groups_;  // P host: control groups
    bool groups_used_ = false;                                               // P host
    ModeUpdateHook before_story_;                                            // P4 host
    ModeUpdateHook after_story_;                                             // OI host
    std::function<void(const StorySwitchControl&)> switch_control_;          // q6: SWITCH_CONTROL reward -> host AI
#ifdef ALAMO_SIM_HAVE_STORY
    lua::GlobalValueStore global_values_;  // Int-final
#endif
    // q6 t06: the GameScoring script
    const FileProvider* scoring_files_ = nullptr;
    std::string scoring_name_;
    std::vector<std::string> scoring_messages_;
    void create_scoring(GameMode& m);
    // q6 t09
    std::string scoring_campaign_;
    std::vector<std::pair<std::string, std::string>> scoring_starts_;  // (mode, map) waiting for the next Service
    std::vector<galactic::GalacticEvent> scoring_events_;              // tapped from the story drains, delivered after T9
    bool scoring_failed_ = false;  // the original's exited flag: a Lua error in an event call / thread
    size_t scoring_seen_messages_ = 0, scoring_seen_errors_ = 0;
    uint64_t scoring_services_ = 0, scoring_calls_ = 0, scoring_releases_ = 0;
    void service_scoring();
    void scoring_flush_starts();
    void scoring_deliver_events();
    bool scoring_call(std::string_view function, const std::vector<lua::AiEventArg>& args);
    void scoring_collect();
    void scoring_release(std::string_view why);
    void scoring_note(std::string_view what);
    uint32_t last_present_ms_ = 0;
    bool have_present_ = false;
    // N1 save / load
    SessionDescriptor session_;
    std::vector<SaveParticipant> participants_;
    void write_frame(ByteWriter& w) const;
    bool read_frame(ByteReader& r);
    void write_objects(ByteWriter& w) const;
    bool read_objects(ByteReader& r);
    void write_galactic(ByteWriter& w) const;
    bool read_galactic(ByteReader& r);
    void write_players(ByteWriter& w) const;
    bool read_players(ByteReader& r);
    void write_globals(ByteWriter& w) const;
    bool read_globals(ByteReader& r);
    void write_spatial(ByteWriter& w) const;
    bool read_spatial(ByteReader& r);
    // q6 t19: "scoring" (the GameScoring script, LUA_SAVE.md) while a scoring script is configured; "object_scripts"
    // (ObjectScriptHost::write / read) while the object-script host exists. Write: false (+ *err) when a VM's Lua state
    // cannot be saved.
    bool scoring_configured() const;
    bool write_scoring(ByteWriter& w, std::string* err) const;
    bool read_scoring(ByteReader& r);
    bool has_tactical_mode() const;
    // N1 stage 2: load pass 1 (before the rebuild) reads "frame" / "objects" for the bottom `load_base_depth_` modes
    // only (0 = every mode, the depth must match).
    uint32_t load_base_depth_ = 0;
    void read_chunks(const SaveFile& in, LoadReport& rep);
#ifdef ALAMO_SIM_HAVE_STORY
    // q6 t03: the scoring VM's own player world while no story session runs (start_galactic; GAME_SCORING.md: the
    // script reaches the session's players in every session). Replaced by the story's world in start_story, dropped
    // with the VM. Before scoring_vm_, so destroyed after it. Null without the players family.
    std::shared_ptr<lua::LuaPlayerWorld> scoring_players_;
    // q6 t06: last member, so it is destroyed before the stores, the RNG and the GlobalValue table it points to.
    std::unique_ptr<lua::LuaVm> scoring_vm_;
#endif
    // q6 object scripts: after the stores, the RNG and the GlobalValue table (destroyed before them); the player world
    // before the host whose VMs point to it.
    std::shared_ptr<lua::LuaPlayerWorld> object_players_;
    std::unique_ptr<ObjectScriptHost> object_scripts_;
};

}  // namespace alamo::sim
