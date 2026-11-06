// P host (alamo_game): one game session = SimHost + galactic campaign + CampaignBattles (tactical battles of the
// conflicts) + sim-owned animation + the human order routing, all on the deterministic tick. The presentation side
// (renderer, HUD, menus, audio) only reads it between ticks and talks to it through commands.
//
//   start()        Start_Game(galactic) with the campaign / skirmish galaxy, CampaignBattles attached (human conflicts
//                  fought or, with ask_battle_choice, decided by a battle_choice_event; AI-vs-AI auto-resolved; A1
//                  tactical AI for the AI side), SimAnimation (T5.anim) attached, the order / port-event hooks
//                  installed, AI players driven by A2's GalacticAiWorld (CampaignAggressor without AI data), the
//                  StorySession for story campaigns, and for a skirmish the human fleet's opening hop as a command.
//   tick()         one SimHost::tick_once; loop_iteration() the paced variant (FrameSynchronizer).
//   submit()       a local-player order (scheduled, recorded): galactic orders go to GalacticSim, tactical orders to
//                  SimHost::apply and, while a battle runs, through the order hook into the battle (battle_orders.hpp).
//   record / replay  CommandRecording of every drained batch; a replay session built from the same SessionSpec and
//                  the recording reproduces the per-tick CRC log (the presentation-on/off check).
// Battle defaults: BattleConfig::skip_human_units (the human's units and reserves take commands only: orders through
// battle_orders.cpp, reinforcements through Reinforce), stand-in order cadence 60 for AI sides without a tactical AI.
//
// N1 / t09 SAVE / LOAD (owner: n2-game; files and descriptions: save_games.hpp; contract: alamo/sim/save_session.hpp):
//   descriptor   start() calls SimHost::set_session: kind "game.campaign" / "game.skirmish", campaign = the CampaignDef /
//                the skirmish map, data root + VFS args, the player list after the AI assignment (faction, human / AI, AI
//                type, difficulty), the difficulty name, options = every SessionSpec field that changes the simulation
//                ("spec.*", "skirmish.*") plus what the build turned on ("run.galactic_ai", "run.story"), and the data
//                fingerprint (object types + galactic rules / setup + the AI inputs, as alamo_headless). Not for the
//                attract demo.
//   participants as alamo_headless save_run: "ai" (GalacticAiWorld write / read) after the chunks CampaignBattles
//                ("galactic.conflicts", "battles") and SimAnimation ("anim") register, then "game" = this session's own
//                state that later ticks read (pause / pause pending, the land skirmish's Invade sent, a battle choice in
//                flight).
//   save         save_blocker() (cheap: a real game, SimHost::save_allowed) -> check_save() (a full SimHost::save: also
//                refused when a chunk is written EMPTY - a system whose save has not landed, e.g. "anim", whose read
//                fails, so the save could not be loaded) -> save() hands that save out (kept while nothing ticks).
//   load         spec_from_save(save) -> GameSession(data, js, spec) -> start_from_save(save): start() exactly like a new
//                game (minus the skirmish's opening order, which the saved command queue holds), SimHost::load (refuses
//                another campaign / seed / players / options / game data, names every failing chunk), then this
//                session's "game" chunk state, the cinematic rate lock re-derived from the loaded story, and a replay
//                (replay_after_load) or a recording (record_after_load) continuing the save. A failed load leaves a
//                half-loaded session: discard it. The loaded session resumes paused exactly when the saved one was
//                paused (the Pause command / the GameSession pause state); the in-game menu's pause is the host's.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/recording.hpp"
#include "alamo/core/save_file.hpp"
#include "alamo/galactic/special_systems.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/save_games.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/sim/sim_host.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::game {

enum class SessionKind : uint8_t { Campaign = 0, Skirmish = 1 };

struct SessionSpec {
    SessionKind kind = SessionKind::Campaign;
    // Campaign: CampaignDef name and the human faction ("" = Starting_Active_Player), difficulty 0..2.
    std::string campaign, faction;
    int32_t difficulty = 1;
    // Skirmish: space map and the galaxy (factions / fleets).
    std::string skirmish_map;
    SkirmishSetup skirmish;
    std::shared_ptr<const terrain::MapData> skirmish_map_data;  // tests: used instead of loading skirmish_map
    // Sim.
    uint32_t seed = 1;
    uint32_t aggressor_period = 600;  // CampaignAggressor for the AI players (0 = off)
    uint32_t battle_max_frames = 27000;
    uint32_t battle_end_hold_frames = 0;  // BattleConfig::end_hold_frames (alamo_game sets it; 0 for tests and the attract demo)
    uint32_t battle_order_interval = 60; // S10 stand-in cadence for AI sides without a tactical AI (human units skipped)
    bool tactical_ai = true;             // A1 BattleAi for the AI players of human battles (needs AI data)
    bool animation = true;               // SimAnimation (needs engine/anim)
    bool galactic_ai = true;             // A2 GalacticAiWorld for the AI players (else / without AI data: CampaignAggressor)
    bool story = true;                   // S4 StorySession for campaigns with story plots (attached to the galactic AI)
    // The pending-battle choice of a human conflict (fight / auto-resolve / retreat): false = always Fight (headless,
    // tests); true = the conflict waits (ConflictManager Vote state) for a battle_choice_event in the command stream.
    bool ask_battle_choice = false;
    // OI host: the main-menu attract demo (docs/re/MAIN_LOOP.md §1.5 / AUDIO.md §1.6 "AI-vs-AI attract demo") [port]: a
    // space skirmish with no human - the skirmish's player 0 is AI controlled too, AI-vs-AI conflicts are fought (both
    // sides by the tactical AI, else the stand-in cadence) and player 0's fleet hop is a direct setup operation (no
    // command). local_player() stays 0 (the viewpoint). Space maps only (a land map fails start()).
    bool attract = false;
};

// The human's answer to a pending conflict as a port-only command (PortExtension subtype kBattleChoiceSubtype, one byte
// = galactic::BattleChoice); GameSession's port hook hands it to ConflictManager::choose at the command phase.
inline constexpr uint16_t kBattleChoiceSubtype = 0x4243;  // "BC"
commands::EventData battle_choice_event(galactic::BattleChoice choice);
// P4 host: the retreat dialog's Autoresolve (IDC_AUTORESOLVE in a tactical battle; retail drives the sim from the GUI
// without a command, docs/re/P4_SIM_SYSTEMS.md §9.3, so the port wraps it in this port event, one byte, for replays).
// The auto-resolve dialog's Withdraw is the original Withdrawl event (COMMANDS.md id 49, no payload, §9.2). The story
// summary's Abandon is dead in retail (§9.1): no command. See GameSession::port_event.
inline constexpr uint16_t kTacticalAutoResolveSubtype = 0x5441;  // "TA"
commands::EventData tactical_auto_resolve_event();
// Difficulty 0..2 -> DifficultyAdjustments name ([port] Easy_Default / Normal_Default / Hard_Default).
const char* difficulty_name(int32_t difficulty);

struct BattleOrderStats {
    uint64_t routed = 0, rejected = 0, moves = 0, attacks = 0, stops = 0, reinforcements = 0;
};
// P4 host: drained Withdraw / tactical auto-resolve events (diagnostics). Withdraw: accepted = Set_Retreating took it,
// dropped = its result was not 0 (the executor ignores it: the command itself always succeeds, §9.2).
struct PortEventStats {
    uint64_t withdraw = 0, withdraw_accepted = 0, withdraw_dropped = 0;
    uint64_t tactical_auto_resolve = 0, tactical_auto_resolve_refused = 0;
    uint64_t story_ui = 0, story_ui_dropped = 0;  // L1/S4 wave 1: story UI events applied / dropped (no story, bad payload)
};

// P4 host: the FoC galactic special dialogs (docs/re/P4_SIM_SYSTEMS.md §1-§5). A human's Ground_Activated hero ability
// posts GalacticEventKind::SpecialDialog instead of applying; for the local player GameSession keeps it here until the
// panel answers (a SpecialAbility command) or closes. Presentation state: never hashed, never read by the sim.
struct SpecialDialog {
    uint32_t kind = 0;                    // galactic::kSpecialSlice / kSpecialBlackMarket / kSpecialSabotage /
                                          // kSpecialCorruption / kSpecialHack
    PlanetIndex planet = kInvalidPlanet;  // planet row
    uint32_t planet_id = 0, hero_id = 0;  // galactic object ids (the hero = slicer / buyer / saboteur / defiler / hacker)
    float respawn_seconds = 0.0f;         // black market / slice: the R the apply drew (dequantized q5; 0 = default respawn)
    uint64_t frame = 0;                   // host frame of the event
};
struct SpecialStats {
    uint64_t dialogs = 0, errors = 0, purchases = 0, sabotages = 0, vigilance = 0, hacks = 0, planets_destroyed = 0,
             respawns_scheduled = 0, respawned = 0, applied = 0;
    uint64_t slices = 0, slice_successes = 0, types_unlocked = 0;  // Slice events (any outcome / success), TypeUnlocked
};

// The slice dialog's ability: the first Slicer_Ability (SpecialAbilityClass::Slicer) in the type's <Abilities> list of
// `hero` (a SpecialRules::abilities index), 0xFFFFFFFF when it has none / is not alive.
uint32_t slicer_ability(const galactic::GalacticSim& g, objects::ObjectHandle hero);
// The slice dialog's rows (GalacticSim::slice_entries, candidate order: type, price, chance) for the slicer `hero_id`
// of `player` at `planet`; empty when the hero is gone or has no Slicer ability (REMAINING_GAPS 1.3 step 3).
std::vector<galactic::SliceEntry> slice_rows(const galactic::GalacticSim& g, int32_t player, PlanetIndex planet, uint32_t hero_id);
// A black-market dialog row can be clicked (L1P4_LUA_GAPS 2.3): not purchased and int(price + 0.5) <= int(credits + 0.5).
bool black_market_row_enabled(const galactic::BlackMarketOffer& o, float credits);

class GameSession {
public:
    // `data` and `js` must outlive the session.
    GameSession(const GameData& data, jobs::JobSystem& js, SessionSpec spec);
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Builds the galaxy and starts the session. `replay` (optional) = a recording to replay instead of live orders
    // (submit() is then ignored by the pipeline); `record` (optional) = where to record. False + diagnostics.
    bool start(std::vector<std::string>* diagnostics = nullptr, const commands::CommandRecording* replay = nullptr,
               commands::CommandRecording* record = nullptr);
    bool started() const { return started_; }

    bool tick();
    bool loop_iteration(uint32_t now_ms);
    commands::SubmitResult submit(commands::EventData ev);
    // Pause / resume (a command-stream entry; see game_session.cpp). True when the request was submitted.
    bool set_paused(bool pause);
    bool paused() const { return paused_; }
    bool pause_requested() const { return paused_ || pause_pending_; }
    uint64_t pause_events() const { return pause_events_; }

    const SessionSpec& spec() const { return spec_; }
    sim::SimHost& host() { return *host_; }
    const sim::SimHost& host() const { return *host_; }
    const galactic::GalacticSim* galaxy() const;
    // GRC 1.3 presentation latch (GalacticSim::consume_reveal_sfx, not hashed): true once after a galactic pre-step
    // revealed a planet on a frame > 0 (the host plays SFXEvent_Planet_Revealed). Finds the galactic mode under a battle.
    bool take_reveal_sfx();
    // GRC 1.1: may the local player order a fleet to this planet object (the human hop gate reads the raw revealed byte
    // D +0x2c9)? True for a non-planet id or without a local player (nothing gated).
    bool move_target_revealed(uint32_t planet_id) const;
    sim::CampaignBattles* battles() { return battles_.get(); }
    const sim::CampaignBattles* battles() const { return battles_.get(); }
    const sim::TacticalBattle* battle() const { return battles_ ? battles_->battle() : nullptr; }
    bool in_battle() const { return battle() != nullptr && host_->modes().top_tactical() != nullptr; }
    int32_t local_player() const { return local_player_; }
    // Does the battle view draw the local player's FOW grid and the HideWhenFogged marks? Not in the attract demo: the
    // original films it with the in-game cinematic camera, and every cinematic shot start (0x49b1a0) clears the FOW render
    // option 0x140a12501 that all fog draw paths test, so the demo shows no fog on any map, with or without a
    // DEMO_CONTROLLER reveal (MAIN_MENU_BACKGROUND.md §4a, [H] code chain, [M] that the capture is that). Presentation
    // only: the demo's sim fog (targeting, AI) is unchanged.
    bool view_draws_fog() const { return !spec_.attract; }
    // Attract demo: the map's Demo_Attract_Controller ran (game_session.cpp attract_demo_script): at least one
    // DEMO_CONTROLLER object revealed the map for Empire / Rebel / Underworld. False before the battle runs and on maps
    // without a controller (the shipped Demo_Attract_Kamino).
    bool attract_controller_ran() const { return attract_controllers_ > 0; }
    uint32_t attract_moves() const { return attract_moves_; }  // its Attack_Move orders the battle accepted
    const BattleOrderStats& order_stats() const { return order_stats_; }
    uint64_t ticks() const { return ticks_; }
    // Skirmish: the battle ran and ended (the session is over).
    bool skirmish_over() const;
    // Human conflict waiting for the battle choice (SessionSpec::ask_battle_choice), its conflict, and the answer (a
    // command; false when nothing is pending or the submit failed). auto_resolving(): a conflict with the local player
    // is being auto-resolved (ConflictManager AutoRunning), `conflict` its head.
    bool battle_choice_pending() const;
    const galactic::ConflictInfo* conflict_head() const;
    bool choose_battle(galactic::BattleChoice choice);
    // GALACTIC_DIALOGS §1.4: the story lock (DISABLE / RESTRICT_AUTORESOLVE, StorySession::autoresolve_locked) forbids
    // auto-resolving a battle at `planet`: the prompt offers Begin only. False without a running story.
    bool autoresolve_locked(PlanetIndex planet) const;
    // story t13 (BT §2.9.3 step 2): the one-button pending banner (story_pending_battle_shell) of a LINK_TACTICAL record with
    // P13 = 1 (CampaignBattles::linked_banner_up, the galaxy paused): its record, and Begin = ResumeGame 51 (a command; the
    // record starts at the next hook). False / null when no banner is up or the Begin is already in flight.
    bool linked_banner_pending() const;
    const sim::CampaignBattles::LinkedRecord* linked_banner_record() const;
    bool begin_linked();
    bool auto_resolving() const;
    // P4 host requests (commands; true when submitted). Their results come at the command phase (port_event):
    // withdraw = Withdrawl 49 -> ConflictManager::set_retreating(issuer), result ignored (P4_SIM_SYSTEMS §9.2);
    // auto_resolve_battle = the tactical IDC_AUTORESOLVE -> CampaignBattles::auto_resolve_battle(issuer) (§9.3).
    // can_withdraw: the Withdraw enable rule (the local player is the head conflict's attacker or defender).
    bool withdraw();
    bool auto_resolve_battle();
    bool can_withdraw() const;
    const PortEventStats& port_stats() const { return port_stats_; }
    // L1/S4 wave 1: a local UI notification for the story (story_ui.hpp) as a command; GameSession::port_event hands it
    // to StorySession::notify_ui with the issuing player at the command phase. False without a story session or when
    // the submit failed.
    bool notify_story_ui(const sim::StoryUiNotification& n);
    // P4 host: galactic special abilities (SpecialAbility event 13; galactic special_systems.hpp). activate_special =
    // kind 1 (a hero or fleet of the local player at the planet). The dialog answers use the open special_dialog() and
    // close it once submitted: buy_black_market (kind 4, item = SpecialRules::items index, price from bm_offers;
    // refuses purchased rows and int(price + 0.5) > int(credits + 0.5); sends StoryUiEvent::BuyBlackMarket with the
    // item Name first, L1P4_LUA_GAPS 2),
    // sabotage_structure (kind 5, cost from sabotage_cost, R 0 as the retail dialog), hack_super_weapon (kind 7,
    // Hack_Super_Weapon_Cost), corrupt_planet (kind 6, apps/sim CampaignBattles), slice_tech (kind 2, the slice_rows
    // row of `type`: its price and chance, the dialog's R, only-one = a single row; REMAINING_GAPS 1.3.1). Cancel =
    // close_special_dialog (nothing sent). False when nothing was submitted.
    bool activate_special(uint32_t unit_id, uint32_t planet_id);
    const SpecialDialog* special_dialog() const { return dialog_open_ ? &dialog_ : nullptr; }
    void close_special_dialog() { dialog_open_ = false; }
    bool slice_tech(uint32_t type);
    bool buy_black_market(uint32_t item);
    bool sabotage_structure(uint32_t structure_id);
    bool hack_super_weapon();
    bool corrupt_planet(int8_t type);
    // The P4 special events (SpecialDialog .. Slice, incl. TypeUnlocked) of every player since the last call, stream order, for the
    // presentation (SFX / messages). Collected right before T9 (the T2..T8 events), from every later batch the story
    // drains inside T9 (events raised by Lua / story rewards there, StorySession::set_drain_tap) and after each tick.
    std::vector<galactic::GalacticEvent> take_special_events();
    const SpecialStats& special_stats() const { return special_stats_; }
    // OI host: the Lua presentation's cinematic rate lock (lua::PresentationState::cinematic_rate_lock: cinematic camera
    // or cinematic mode of the story VMs), read right after T9 of every tick (SimHost::set_after_story_hook): it pins
    // the pacing to 30 ticks/s (FrameSynchronizer::set_cinematic_lock) and selects the parity CRC's non-inverted form
    // for this tick's capture (ParityFrameOptions::invert = !lock, COMMANDS.md §7.1 step 6). Sim-computed input, so a
    // replay gets the same parity. False without a story session.
    bool cinematic_rate_lock() const { return rate_lock_; }
    uint64_t rate_lock_ticks() const { return rate_lock_ticks_; }  // ticks captured with the lock (diagnostics)
    // Tests: replaces the story presentation as the lock's source (nullptr = back to the story).
    void set_rate_lock_source(std::function<bool()> fn) { rate_lock_source_ = std::move(fn); }
    // refixf t05: the in-game cinematic camera's lock (TSW firing shot, TSW_PLANET_DESTROY.md §11.2 step 2: FS +0x37 while
    // the shot runs). Pacing only (30 ticks/s, wins over fast mode): [port] the parity options stay with the story lock,
    // because the shot is presentation (input can skip it) and must not reach a CRC.
    void set_presentation_rate_lock(bool on);
    bool presentation_rate_lock() const { return pres_lock_; }
    // OI host: single-player fast mode (the HUD's b_fast_forward / b_fast_forward_t; MAIN_LOOP.md §1.5: 120 ticks/s,
    // the cinematic lock wins). Wall-clock pacing only (FrameSynchronizer::set_fast), mirrored into the Lua presentation
    // state (PresentationLog::set_fast_forward); Lua Cancel_Fast_Forward ends it at the next tick; a pause / resume
    // clears it. Never changes what a tick computes.
    void set_fast_forward(bool on);
    bool fast_forward() const { return host_ && host_->pacing().fast(); }
    // AI / story state (diagnostics).
    bool galactic_ai_running() const;
    bool story_running() const { return host_ && host_->story() != nullptr; }
    uint64_t galactic_ai_orders() const;
#ifdef ALAMO_SIM_HAVE_ANIM
    const sim::SimAnimation* animation() const { return anim_.get(); }
#endif
    const GameData& data() const { return *data_; }

    // ---- N1 / t09 save / load (see the file comment) ----
    static constexpr const char* kCampaignKind = "game.campaign";
    static constexpr const char* kSkirmishKind = "game.skirmish";
    // "" when this session may be saved now, else why not (no session, the attract demo, a replay, SimHost::save_allowed).
    std::string save_blocker() const;
    // save_blocker(), then a full SimHost::save: "" when the save would load, else why not ("anim: pending ..."). The
    // save is kept for save() until the session ticks or takes a command.
    std::string check_save();
    // The session as a save (description = encode_description(meta)). False + *err with check_save()'s reason.
    bool save(SaveFile& out, const SaveMeta& meta, std::string* err = nullptr);
    // The description fields of this session now (name and wall-clock time from the caller; title = the campaign name).
    SaveMeta save_meta(std::string name, int64_t saved_at) const;
    // The SessionSpec a save was written with (false + *err: no session chunk, not an alamo_game save, bad options).
    // Tests' in-memory skirmish maps (skirmish_map_data) are not saved: the caller sets them again.
    static bool spec_from_save(const SaveFile& save, SessionSpec& out, std::string* err = nullptr);
    // Instead of start(): rebuild + SimHost::load + this session's state. `replay` (optional) = a stream recorded after
    // this save was loaded (or a full stream of the original run, commands/recording.hpp); `record` (optional) records
    // a stream continuing the save. False + *err (the LoadReport line): discard the session.
    bool start_from_save(const SaveFile& save, std::string* err = nullptr, const commands::CommandRecording* replay = nullptr,
                         commands::CommandRecording* record = nullptr);
    // Records the drained batches from now on (call before the first tick; a loaded session's stream continues its save).
    void record_to(commands::CommandRecording* rec);
    bool loaded() const { return loaded_; }
    const commands::SavePoint& load_point() const { return load_point_; }  // {save tick, save digest} of the loaded save
    const sim::LoadReport& load_report() const { return load_report_; }

private:
    const GameData* data_;
    jobs::JobSystem* js_;
    SessionSpec spec_;
#ifdef ALAMO_SIM_HAVE_ANIM
    std::unique_ptr<sim::SimAnimation> anim_;  // before host_: outlives the host's hooks / CRC hashers
#endif
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    std::unique_ptr<ai::GalacticAiWorld> galactic_ai_;  // before host_: its "ai.goals" CRC hasher is in the host's registry
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    std::vector<std::vector<ai::tactical::TacticalBudgetCategory>> budget_rows_;  // A1 land-battle credit transfer
#endif
#endif
    std::unique_ptr<sim::SimHost> host_;
    std::unique_ptr<sim::CampaignBattles> battles_;  // after host_: destroyed first (clears its hooks)
    galactic::GalacticSetup setup_;
    terrain::MapData skirmish_map_;
    int32_t local_player_ = 0;
    bool started_ = false;
    std::array<uint32_t, 2> attract_sides_{};  // attract demo: attacker / defender faction = player index (§6)
    bool attract_script_done_ = false;          // attract demo: the controller script's State_Init point has passed
    uint32_t attract_controllers_ = 0;          // DEMO_CONTROLLER objects whose script ran
    uint32_t attract_moves_ = 0;                // their Attack_Move orders the battle accepted
    bool replay_ = false, invaded_ = false;
    bool paused_ = false, pause_pending_ = false;
    uint64_t pause_events_ = 0;
    bool choice_sent_ = false;  // a battle choice is in flight for the pending conflict
    bool banner_sent_ = false;  // story t13: the linked banner's Begin is in flight (cleared once the banner is down)
    PortEventStats port_stats_;
    commands::RejectReason port_event(uint8_t player, const commands::EventData& ev);
    void land_skirmish_invade();
    // P4 host: galactic special events (presentation tap) and the open dialog.
    void tap_events(bool story_drains);
    void tap_event(const galactic::GalacticSim& gs, const galactic::GalacticEvent& e);
    void after_tick();
    // OI host: after-T9 hook (rate lock -> pacing + parity) and its state.
    void after_story(sim::SimHost& h);
    void attract_demo_script(sim::SimHost& h);
    std::function<bool()> rate_lock_source_;
    bool rate_lock_ = false;
    bool pres_lock_ = false;  // refixf t05 (pacing only)
    uint64_t rate_lock_ticks_ = 0;
    size_t events_seen_ = 0;  // GalacticSim::events() already tapped
    std::vector<galactic::GalacticEvent> special_events_;
    SpecialDialog dialog_;
    bool dialog_open_ = false;
    SpecialStats special_stats_;
    bool submit_special(commands::EventData ev);
    uint64_t ticks_ = 0;
    BattleOrderStats order_stats_;
    // N1 / t09 save / load.
    bool loading_ = false;  // start() runs for start_from_save (no skirmish opening order)
    bool loaded_ = false;
    commands::SavePoint load_point_;
    sim::LoadReport load_report_;
    std::vector<std::string> ai_types_;  // per player: the galactic AI type ("" = none), for the descriptor
    uint64_t submits_ = 0;               // commands submitted (check_save's cache key with the host frame)
    std::optional<SaveFile> save_cache_;
    std::string save_refusal_;           // check_save()'s cached answer when it refused
    bool save_checked_ = false;
    uint64_t save_check_frame_ = 0, save_check_submits_ = 0;
    void describe_session();             // SimHost::set_session (end of start())
    void write_game_state(ByteWriter& w) const;
    bool read_game_state(ByteReader& r);
    void sync_rate_lock(sim::SimHost& h);  // the cinematic rate lock -> pacing + parity, from the current story state
};

// ---- N1 / t09: the menus' save / load paths (alamo_game's Save / Load dialogs and the save tests use these) ----
// check_save -> save(save_meta(name, saved_at), title shown instead of the campaign name when non-empty) ->
// write_save_file(save_path(dir, name)) (overwrites). False + *err (check_save's reason or the write error);
// *path_out = the file written.
bool save_game(GameSession& session, const std::string& dir, std::string_view name, const std::string& title, int64_t saved_at,
               std::string* path_out = nullptr, std::string* err = nullptr);
// SaveFile::read_file(path) -> spec_from_save -> a new GameSession -> start_from_save (replay / record as there). Null +
// *err when the file is not a save of this game, belongs to other game data / options (SessionDescriptor::mismatch) or a
// chunk does not load; the half-loaded session is discarded. `file_out` (optional) gets the save read.
std::unique_ptr<GameSession> load_game(const GameData& data, jobs::JobSystem& js, const std::string& path, std::string* err = nullptr,
                                       const commands::CommandRecording* replay = nullptr, commands::CommandRecording* record = nullptr,
                                       SaveFile* file_out = nullptr);

}  // namespace alamo::game
