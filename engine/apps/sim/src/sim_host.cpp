#include "alamo/sim/sim_host.hpp"

#include <algorithm>
#include <limits>

#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/ai_bridge.hpp"         // q6 t09: call_global_function / AiEventArg (GameScoring event calls)
#include "alamo/lua/object_bridge.hpp"     // q6 t09: the story's families for the scoring VM
#include "alamo/lua/presentation.hpp"
#include "alamo/lua/script_remaining.hpp"  // L1 bindings: process-wide GlobalValue store
#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/lua/players.hpp"
#endif
#endif

namespace alamo::sim {

namespace cmd = alamo::commands;

namespace {
bool finite(float f) { return f - f == 0.0f; }
}  // namespace

SimHost::SimHost(const objects::GameObjectTypes& types, jobs::JobSystem& js, SimConfig cfg)
    : js_(&js),
      cfg_(cfg),
      pf_(jobs::make_parallel_for(js)),
      modes_(types, cfg.players < kMaxPlayers ? cfg.players : kMaxPlayers) {
    if (cfg_.players > kMaxPlayers) cfg_.players = kMaxPlayers;
    // Q6: every player may submit orders (a mod's campaign can have more than the scheduler's default 8). Only raised, so
    // sessions of up to 8 players keep their recorded scheduler config.
    if (cfg_.scheduler.max_players < cfg_.players) cfg_.scheduler.max_players = static_cast<uint8_t>(cfg_.players);
    cfg_.scheduler.first_tick = 1;  // the first drain is tick 1 (T2 runs after the frame increment)
    random_.seed_free(cfg_.free_seed);
    if (cfg_.object_scripts) {  // q6: game-object Lua scripts (object_scripts.hpp)
        object_scripts_ = std::make_unique<ObjectScriptHost>(random_);
#ifdef ALAMO_SIM_HAVE_STORY
        object_scripts_->set_global_values(&global_values_);
#endif
    }
    make_pipeline(cfg_.scheduler);
    register_crc();
    make_recorder();
}

void SimHost::make_pipeline(const cmd::SchedulerConfig& cfg) {
    pipeline_ = std::make_unique<cmd::CommandPipeline>(cfg);
    pipeline_->set_handler([this](uint64_t tick, const cmd::Command& c) { return handle(tick, c); });
    pipeline_->set_crc_hook([this](const void* data, size_t size) {
        StateHasher h;
        h.bytes(data, size);
        batch_crc_ = h.digest();
    });
    if (record_) pipeline_->record_to(record_);
}

void SimHost::register_crc() {
    registry_.add("frame", [this](StateHasher& h) {
        h.u64(frame_);
        const auto depth = static_cast<uint32_t>(modes_.depth());
        h.u32(depth);
        for (uint32_t i = depth; i-- > 0;) modes_.at(i).hash(h);  // bottom -> top
#ifdef ALAMO_SIM_HAVE_STORY
        if (global_values_.size() != 0) global_values_.hash(h);  // Int-final: the session GlobalValue table, once used
#endif
    });
    registry_.add("objects", [this](StateHasher& h) {
        const auto depth = static_cast<uint32_t>(modes_.depth());
        h.u32(depth);
        for (uint32_t i = depth; i-- > 0;) {
            h.u8(static_cast<uint8_t>(modes_.at(i).kind()));
            modes_.at(i).primary().hash_state(h);
        }
    });
    registry_.add("galactic.state", [this](StateHasher& h) {
        const GalacticMode* g = modes_.empty() ? nullptr : dynamic_cast<const GalacticMode*>(&modes_.at(modes_.depth() - 1));
        const galactic::GalacticSim* s = g ? g->sim() : nullptr;
        h.boolean(s != nullptr);
        if (s) s->hash_state(h);
    });
    registry_.add("spatial", [this](StateHasher& h) {
        const TacticalMode* tm = modes_.top_tactical();
        h.boolean(tm != nullptr);
        if (tm) tm->trees().hash_state([&h](const void* d, size_t n) { h.bytes(d, n); });
    });
    registry_.add("players", [this](StateHasher& h) {
        h.u32(cfg_.players);
        for (uint32_t p = 0; p < cfg_.players; ++p) {
            h.u32(static_cast<uint32_t>(selection_[p].size()));
            for (ObjectId id : selection_[p]) h.i32(id);
        }
        if (groups_used_)  // P host: control groups, once assigned
            for (uint32_t p = 0; p < cfg_.players; ++p)
                for (const std::vector<ObjectId>& g : groups_[p]) {
                    h.u32(static_cast<uint32_t>(g.size()));
                    for (ObjectId id : g) h.i32(id);
                }
    });
    registry_.add("rng", [this](StateHasher& h) { random_.hash(h); });
    registry_.add("commands", [this](StateHasher& h) { h.u64(batch_crc_); });
    if (cfg_.story_crc)
        registry_.add("story", [this](StateHasher& h) {
            h.boolean(story_ != nullptr);
            if (story_) story_->hash(h);
        });
    registry_.add_parity("objects.parity", [this] {
        const GameMode* t = modes_.top();
        return t ? t->primary().parity_crc() : 0u;
    });
    registry_.add_parity("rng.parity", [this] { return parity_rng_crc(random_.synced().state()); });
}

// ------------------------------------------------------------------------------------- mode stack
GameMode& SimHost::start_game(ModeKind kind) {
    if (modes_.empty()) {
        // New session: FrameSynchronizer reset (frame 0), synced RNG seeded, id counter reset.
        story_.reset();
        if (session_used_) {
            frame_ = 0;
            batch_crc_ = 0;
            make_pipeline(cfg_.scheduler);
            make_recorder();
            for (auto& s : selection_) s.clear();
            for (auto& pg : groups_)  // P host: control groups
                for (auto& g : pg) g.clear();
            groups_used_ = false;
        }
        session_used_ = true;
#ifdef ALAMO_SIM_HAVE_STORY
        lua::process_global_values().clear();  // L1 bindings: GlobalValue is cleared for a new game (L1_S4_REMAINING §1.5)
        global_values_.clear();                // Int-final: the session-wide table too
#endif
        pacing_.reset();
        random_.seed_session(cfg_.seed);
        modes_.reset_ids();
    }
    const bool new_session = modes_.empty();
    GameMode& m = modes_.push(kind);
    // --- OI objects: behaviour Initialise draws its first due frame from the synced RNG (TACTICAL_GAPS 6.4);
    //     primary store only (the secondary store is local / cosmetic) ---
    m.primary().bind_sync_rng(&random_.synced());
    // --- end OI objects ---
    if (new_session) create_scoring(m);  // q6 t06: before the players are set up and the plots load
    if (object_scripts_) {  // q6: scripts attach to every object the mode's primary store creates from now on
        if (new_session) {
            object_players_.reset();
            object_scripts_->set_campaign(kind == ModeKind::Galactic);
        }
        object_scripts_->mode_pushed(m);
    }
    pacing_.select_active(m.tactical());
    pacing_.set_stopped(false);
    if (story_ && m.tactical()) story_->host_event(StoryHostEvent::TacticalStarted, frame_);
    return m;
}

void SimHost::set_object_script_files(const FileProvider* files) {
    if (object_scripts_) object_scripts_->set_files(files);
}

void SimHost::set_scoring_script(const FileProvider* files, std::string name) {
    scoring_files_ = files;
    scoring_name_ = std::move(name);
}

const lua::LuaVm* SimHost::scoring_vm() const {
#ifdef ALAMO_SIM_HAVE_STORY
    return scoring_vm_.get();
#else
    return nullptr;
#endif
}

// GAME_SCORING.md §1.1 / §1.2: release the old script, then a fresh unpooled state: the file's top level (require),
// Base_Definitions(), the main thread (created, not resumed). Errors are kept as messages; the session goes on without
// the script's globals.
void SimHost::create_scoring(GameMode& m) {
#ifdef ALAMO_SIM_HAVE_STORY
    scoring_vm_.reset();
    scoring_players_.reset();  // q6 t03: start_galactic attaches a new one
    scoring_starts_.clear();
    scoring_events_.clear();
    scoring_failed_ = false;
    scoring_seen_messages_ = scoring_seen_errors_ = 0;
    scoring_services_ = scoring_calls_ = scoring_releases_ = 0;
    if (scoring_name_.empty() || !scoring_files_) return;
    auto note = [this](std::string_view what) { scoring_note(what); };
    lua::LuaHost host;
    host.store = &m.primary();
    host.types = &m.types();
    host.rng = &random_;
    host.files = scoring_files_;
    host.campaign = m.kind() == ModeKind::Galactic;
    lua::LuaVmOptions opt;
    opt.script_name = scoring_name_;
    auto vm = std::make_unique<lua::LuaVm>(host, opt);
    lua::attach_global_values(*vm, &global_values_);
    std::string err;
    if (!vm->require(scoring_name_, &err)) note("require: " + err);
    else if (vm->has_function("Base_Definitions") && !vm->call("Base_Definitions", &err)) note("Base_Definitions: " + err);
    vm->create_thread("main");  // missing main: no thread (§1.1 step 5)
    scoring_vm_ = std::move(vm);
    scoring_collect();
#else
    (void)m;
#endif
}

void SimHost::scoring_note(std::string_view what) {
    scoring_messages_.push_back(std::to_string(frame_) + " " + scoring_name_ + ": " + std::string(what));
}

// q6 t09 (GAME_SCORING.md §2 / §3). Everything below runs on the sim thread at the fixed points listed in sim_host.hpp.
#ifdef ALAMO_SIM_HAVE_STORY
namespace {
constexpr const char* scoring_mode_name(ModeKind k) {
    return k == ModeKind::Galactic ? "Galactic" : k == ModeKind::Land ? "Land" : "Space";  // [M strings]
}
lua::AiEventArg arg_string(std::string s) {
    lua::AiEventArg a;
    a.kind = lua::AiEventArg::String;
    a.text = std::move(s);
    return a;
}
lua::AiEventArg arg_player(int32_t p) {
    lua::AiEventArg a;
    a.kind = lua::AiEventArg::Player;
    a.id = p;
    return a;
}
lua::AiEventArg arg_object(objects::ObjectHandle h) {
    lua::AiEventArg a;
    a.kind = lua::AiEventArg::Object;
    a.object = h;
    return a;
}
lua::AiEventArg arg_type(uint32_t t) {
    lua::AiEventArg a;
    a.kind = lua::AiEventArg::Type;
    a.type = t;
    return a;
}
}  // namespace
#endif

// New lines of the VM's message / error logs -> scoring_messages(); a new thread error is the exited flag.
void SimHost::scoring_collect() {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!scoring_vm_) return;
    const std::vector<std::string>& msgs = scoring_vm_->messages();
    for (; scoring_seen_messages_ < msgs.size(); ++scoring_seen_messages_) scoring_note(msgs[scoring_seen_messages_]);
    const std::vector<std::string>& errs = scoring_vm_->errors();
    for (; scoring_seen_errors_ < errs.size(); ++scoring_seen_errors_) {
        scoring_note("error: " + errs[scoring_seen_errors_]);
        scoring_failed_ = true;
    }
#endif
}

void SimHost::scoring_release(std::string_view why) {
#ifdef ALAMO_SIM_HAVE_STORY
    scoring_note(std::string("released (") + std::string(why) + ")");
    scoring_vm_.reset();
    scoring_players_.reset();
    scoring_starts_.clear();
    scoring_events_.clear();
    ++scoring_releases_;
#else
    (void)why;
#endif
}

// §3: by global name; missing = skipped; protected, synchronous, not a thread. An error is the exited flag (§2 step 5).
bool SimHost::scoring_call(std::string_view function, const std::vector<lua::AiEventArg>& args) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!scoring_vm_ || !scoring_vm_->has_function(function)) return false;
    scoring_vm_->set_time(static_cast<double>(static_cast<float>(frame_) / static_cast<float>(kSimTicksPerSecond)));
    scoring_vm_->set_frame(static_cast<uint32_t>(frame_));
    std::string err;
    ++scoring_calls_;
    const bool ok = lua::call_global_function(*scoring_vm_, function, args, &err);
    if (!ok) {
        scoring_note(std::string(function) + ": " + (err.empty() ? std::string("error") : err));
        scoring_failed_ = true;
    }
    scoring_collect();
    return ok;
#else
    (void)function;
    (void)args;
    return false;
#endif
}

void SimHost::scoring_mode_started(ModeKind kind, std::string map_name) {
#ifdef ALAMO_SIM_HAVE_STORY
    if (scoring_vm_) scoring_starts_.emplace_back(scoring_mode_name(kind), std::move(map_name));
#else
    (void)kind;
    (void)map_name;
#endif
}

void SimHost::scoring_reset_tactical_stats() {
#ifdef ALAMO_SIM_HAVE_STORY
    scoring_call("Reset_Tactical_Stats", {});
#endif
}

void SimHost::scoring_flush_starts() {
#ifdef ALAMO_SIM_HAVE_STORY
    std::vector<std::pair<std::string, std::string>> starts;
    starts.swap(scoring_starts_);
    for (auto& [mode, map] : starts) {
        if (mode == "Galactic" && map.empty()) map = scoring_campaign_.empty() ? session_.campaign : scoring_campaign_;
        scoring_call("Game_Mode_Starting_Event", {arg_string(mode), arg_string(map)});
    }
#endif
}

// §2 Service, at GameModeManager::update's "gmm.scoring" step.
void SimHost::service_scoring() {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!scoring_vm_) return;
    scoring_flush_starts();
    if (!scoring_vm_) return;
    lua::LuaVm& vm = *scoring_vm_;
    double rate = 0.0;
    if (!vm.get_number("ServiceRate", rate)) return;  // no ServiceRate: no threads ever run
    const double now = static_cast<double>(static_cast<float>(frame_) / static_cast<float>(kSimTicksPerSecond));
    double last = 0.0;
    if (vm.get_number("LastService", last) && !(now - last > rate)) return;
    if (!scoring_failed_ && !vm.exit_requested()) {
        vm.set_time(now);
        vm.set_frame(static_cast<uint32_t>(frame_));
        vm.service_threads();
        ++scoring_services_;
        scoring_collect();
    }
    if (scoring_failed_ || vm.exit_requested()) {
        scoring_release(scoring_failed_ ? "Lua error" : "ScriptExit");
        return;
    }
    vm.set_number("LastService", now);
#endif
}

// After T9: the galactic events the story drained this tick (stream order).
void SimHost::scoring_deliver_events() {
#ifdef ALAMO_SIM_HAVE_STORY
    if (scoring_events_.empty()) return;
    std::vector<galactic::GalacticEvent> events;
    events.swap(scoring_events_);
    GalacticMode* g = galactic_mode();
    const galactic::GalacticSim* gs = g ? g->sim() : nullptr;
    if (!gs) return;
    const objects::ObjectStore& st = gs->store();
    using K = galactic::GalacticEventKind;
    auto planet_object = [gs](PlanetIndex p) {
        return p < gs->planets().size() ? gs->planets()[p].object : objects::ObjectHandle{};
    };
    auto player_or_neutral = [gs](int32_t p) {
        return p >= 0 && static_cast<size_t>(p) < gs->players().size() ? p : gs->neutral_player();
    };
    auto object_of = [&st](int32_t id) { return id > 0 ? st.find(static_cast<ObjectId>(id)) : objects::ObjectHandle{}; };
    for (const galactic::GalacticEvent& e : events) {
        if (!scoring_vm_) return;
        switch (e.kind) {
            case K::PlanetOwnerChanged:
                if (e.player != e.other_player)
                    scoring_call("Galactic_Planet_Faction_Change", {arg_object(planet_object(e.planet)),
                                                                    arg_player(player_or_neutral(e.player)),
                                                                    arg_player(player_or_neutral(e.other_player))});
                break;
            case K::ProductionBegin:
                scoring_call("Galactic_Production_Begin_Event", {arg_object(planet_object(e.planet)), arg_type(e.type)});
                break;
            case K::ProductionCanceled:
                scoring_call("Galactic_Production_Canceled_Event", {arg_object(planet_object(e.planet)), arg_type(e.type)});
                break;
            case K::ProductionCompleted: {
                objects::ObjectHandle h = object_of(e.object);
                if (st.alive(h)) {
                    const objects::ObjectHandle team = st.container(h);
                    if (st.alive(team)) h = team;
                }
                scoring_call("Galactic_Production_End_Event",
                             {arg_object(planet_object(e.planet)), st.alive(h) ? arg_object(h) : arg_type(e.type)});
                break;
            }
            case K::ObjectDestroyed: {
                const objects::ObjectHandle h = object_of(e.object);
                if (st.alive(h))
                    scoring_call("Galactic_Unit_Destroyed_Event", {arg_object(h), arg_player(gs->neutral_player())});
                break;
            }
            default: break;
        }
    }
#endif
}

lua::GlobalValueStore* SimHost::global_values() {  // Int-final
#ifdef ALAMO_SIM_HAVE_STORY
    return &global_values_;
#else
    return nullptr;
#endif
}

bool SimHost::story_supported() {
#ifdef ALAMO_SIM_HAVE_STORY
    return true;
#else
    return false;
#endif
}

bool SimHost::start_story(const StoryStart& start, std::vector<std::string>* diagnostics) {
    GalacticMode* g = galactic_mode();
    if (!g || !g->sim() || !story_supported()) {
        if (diagnostics) diagnostics->push_back(story_supported() ? "story: no galactic campaign running"
                                                                  : "story: built without engine/lua");
        return false;
    }
    auto s = std::make_unique<StorySession>(*g->sim(), g->primary(), g->types(), random_);
    s->set_command_pipeline(pipeline_.get());  // orders issued while the scripts load
    s->set_lua_gc_parallel_for(pf_);            // OPT2 t04: scheduled Lua collections on the host's job system
    s->set_switch_control_hook(switch_control_);  // q6: before the plots load (sim_host.hpp)
    g->sim()->set_emit_events(true);
    StoryStart st = start;  // Int-final: the session-wide GlobalValue table for every plot VM
    if (!st.global_values) st.global_values = global_values();
    if (!s->start(st, diagnostics)) {
        g->sim()->set_emit_events(false);
        return false;
    }
    story_ = std::move(s);
#ifdef ALAMO_SIM_HAVE_STORY
    // q6 t09: the scoring VM reaches the session's players, objects and presentation like the plot scripts, and gets the
    // galactic events every T9 drain takes from the stream (delivered after T9).
    if (scoring_vm_) {
#if defined(ALAMO_LUA_PLAYERS)
        lua::attach_player_world(*scoring_vm_, story_->player_world());
#endif
        lua::attach_object_bridge(*scoring_vm_, story_->object_bridge());
        lua::attach_presentation(*scoring_vm_, story_->presentation());
    }
    scoring_players_.reset();  // q6 t03: the story's world replaces the scoring VM's own
    if (object_scripts_) {     // q6: the galactic objects' scripts get the story's world and object bridge
#if defined(ALAMO_LUA_PLAYERS)
        object_scripts_->set_player_world(g->primary(), story_->player_world());
#endif
        object_scripts_->set_bridge(g->primary(), story_->object_bridge());
    }
    story_->set_scoring_tap([this](const std::vector<galactic::GalacticEvent>& events) {
        if (scoring_vm_) scoring_events_.insert(scoring_events_.end(), events.begin(), events.end());
    });
#endif
    return true;
}

GalacticMode* SimHost::galactic_mode() {
    return modes_.empty() ? nullptr : dynamic_cast<GalacticMode*>(&modes_.at(modes_.depth() - 1));
}

galactic::GalacticSim* SimHost::start_galactic(const galactic::GalacticRules& rules, const galactic::GalacticSetup& setup,
                                               std::vector<std::string>* diagnostics) {
    if (setup.players.size() > cfg_.players) {
        if (diagnostics)
            diagnostics->push_back("galactic setup has " + std::to_string(setup.players.size()) + " players, the session " +
                                   std::to_string(cfg_.players) + " (SimConfig::players, at most " + std::to_string(kMaxPlayers) + ")");
        return nullptr;
    }
    auto& mode = static_cast<GalacticMode&>(start_game(ModeKind::Galactic));
    auto sim = std::make_unique<galactic::GalacticSim>(rules, mode.primary(), random_.synced());
    sim->set_parallel_for(pf_);
    if (!sim->start(setup, diagnostics)) {
        end_game_mode();
        return nullptr;
    }
    mode.attach_sim(std::move(sim));
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_LUA_PLAYERS)
    // q6 t03 (GAME_SCORING.md §3, the PlayerWrapper arguments; TR's plugins call Find_Player / Get_Name): until a story
    // session gives it the story's world, the scoring VM gets a player world of its own over the campaign's players. [port]
    // Not hashed: with a GalacticSim the facts it serves (credits, relations, tech locks) are the sim's, which the sim
    // hashes; only Lua-only flags a scoring script might set live here (the story session's world hashes those).
    if (scoring_vm_ && !story_) {
        scoring_players_ = std::make_shared<lua::LuaPlayerWorld>();
        scoring_players_->attach_galactic(mode.sim());
        lua::attach_player_world(*scoring_vm_, scoring_players_.get());
    }
#endif
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_LUA_PLAYERS)
    // q6 object scripts: the galactic objects' scripts (attached while the sim set the galaxy up) and the battle hosts'
    // tactical scripts without a story session reach the campaign's players through a world of their own [port].
    if (object_scripts_ && !story_) {
        object_players_ = std::make_shared<lua::LuaPlayerWorld>();
        object_players_->attach_galactic(mode.sim());
        object_scripts_->set_player_world(mode.primary(), object_players_.get());
    }
#endif
    scoring_mode_started(ModeKind::Galactic, {});  // q6 t09: map = the campaign name, resolved at delivery
    return mode.sim();
}

bool SimHost::end_game_mode() {
#ifdef ALAMO_SIM_HAVE_STORY
    if (scoring_vm_ && modes_.top()) {  // q6 t09: Game_Mode_Ending_Event before the mode's shutdown (§3, signal 0x24)
        scoring_flush_starts();          // a mode set up and ended within one frame still started first
        scoring_call("Game_Mode_Ending_Event", {arg_string(scoring_mode_name(modes_.top()->kind()))});
    }
    if (modes_.depth() <= 1) {  // q6 t06: before the store its wrappers point into
        scoring_vm_.reset();
        scoring_players_.reset();  // q6 t03: after the VM that points to it
    }
#endif
    if (object_scripts_ && modes_.top()) {  // q6: the mode's object scripts go before its store
        object_scripts_->mode_popping(*modes_.top());
        if (modes_.depth() <= 1) object_players_.reset();
    }
    if (story_) {
        if (modes_.depth() <= 1) story_.reset();  // the galactic mode (bottom) ends: the story with it
        else if (modes_.top() && modes_.top()->tactical()) story_->host_event(StoryHostEvent::TacticalEnded, frame_);
    }
    if (!modes_.pop()) return false;
    if (modes_.empty()) pacing_.set_stopped(true);
    else pacing_.select_active(modes_.top()->tactical());
    return true;
}

// ------------------------------------------------------------------------------------------- loop
bool SimHost::tick_once() {
    if (modes_.empty()) return false;
    if (recorder_->log().records.empty()) recorder_->capture(frame_);  // initial state (after load)
    // T1 Begin_Tick.
    ++frame_;
    profile::begin_tick(frame_);  // OPT1: wall-clock profiler; nothing below reads it
    step("T1.begin_tick");
    // T2 scheduled commands due this frame (handler runs inside, in drain order).
    batch_crc_ = 0;
    pipeline_->begin_tick(frame_);
    step("T2.commands");
    // T4 TreeCull: rebuild dirty spatial trees.
    if (TacticalMode* tm = modes_.top_tactical()) tm->trees().maintain(sim_time(), pf_);
    step("T4.treecull");
    // T5 GameModeManager::Update (only the top mode ticks).
    ModeTickContext ctx;
    ctx.global_frame = frame_;
    ctx.random = &random_;
    ctx.pf = &pf_;
    ctx.players = cfg_.players;
    ctx.trace = trace_;
#ifdef ALAMO_SIM_HAVE_STORY
    if (scoring_vm_) ctx.scoring = [this] { service_scoring(); };  // q6 t09: GAME_SCORING.md §2
#endif
    if (object_scripts_) ctx.object_scripts = [this](GameMode& m) { object_scripts_->service(m); };  // q6
    modes_.update(ctx);
    step("T5.gmm_update");
    if (after_mode_update_) {  // S10: tactical battle of a galactic conflict (campaign_battles.hpp)
        after_mode_update_(*this);
        step("T5.battles");
    }
    if (object_anim_) {  // S10-int: sim-owned animation (sim_anim.hpp)
        object_anim_(*this);
        step("T5.anim");
    }
    // T6 scene animations [stub].
    // T7 PlayerList update in player index order (AI controllers issue orders here).
    static const char* const kPlayerZone[] = {"T7.p0", "T7.p1", "T7.p2", "T7.p3", "T7.p4", "T7.p5", "T7.p6", "T7.p7", "T7.p8+"};
    for (uint32_t p = 0; p < cfg_.players; ++p) {
        if (controllers_[p]) controllers_[p](*this, static_cast<uint8_t>(p), frame_);
        profile::lap(kPlayerZone[p < 8 ? p : 8]);  // per-player AI / controller time
    }
    step("T7.players");
    // --- P4 host (owner: game host agent): presentation tap before the story drains the galactic events ---
    if (before_story_) before_story_(*this);
    // --- end P4 host ---
    // T8 global Lua [stub]. T9 story (S4): story time = galactic mode time (GALACTIC_ECONOMY 8) = the T9 time argument
    // (float)(mode frame) x seconds-per-frame (STORY.md 4.7 / 7.3), a pure function of the synced frame count.
    if (story_) {
        const GalacticMode* g = galactic_mode();
        const double mode_seconds = StorySession::t9_seconds(g ? g->frame() : frame_);
        story_->set_command_pipeline(pipeline_.get());  // script orders: execute_direct at this frame
        story_->service(frame_, mode_seconds);
        step("T9.story");
    }
#ifdef ALAMO_SIM_HAVE_STORY
    if (scoring_vm_) {  // q6 t09: GameScoring galactic event calls (the events this T9 drained, stream order)
        scoring_deliver_events();
        step("T9.scoring");
    }
#endif
    // --- OI host (owner: game host agent): pacing / parity inputs from this tick's presentation state ---
    if (after_story_) after_story_(*this);
    // --- end OI host ---
    // T10 tutorial, T11 campaign end [stubs].
    // T12 Post_Tick.
    modes_.post_tick();
    step("T12.post_tick");
    // T13 End_Tick: events, frame CRC.
    pipeline_->events().deliver();
    recorder_->capture(frame_);
    step("T13.end_tick");
    profile::end_tick();
    return true;
}

uint64_t SimHost::run_fast(uint64_t ticks) {
    uint64_t n = 0;
    while (n < ticks && tick_once()) ++n;
    return n;
}

bool SimHost::loop_iteration(uint32_t now_ms) {
    pacing_.set_stopped(modes_.empty());
    // [port] no per-frame mode update (vtable +0xb8, R1 d = 0.03 per tick): no mode overrides it (cu9 t15).
    return pacing_.pace(now_ms) && tick_once();
}

uint64_t SimHost::run_realtime(uint64_t ticks, const std::function<uint32_t()>& now_ms,
                               const std::function<void(uint32_t)>& sleep_ms, uint32_t fps_cap,
                               const std::function<void()>& after_tick) {
    const uint32_t cap = fps_cap < 15u ? 15u : fps_cap;
    const uint32_t target = 1000u / cap - 1u;  // FPS cap: floor(1000 / limit) - 1 ms
    uint64_t done = 0;
    while (done < ticks && !modes_.empty()) {
        if (loop_iteration(now_ms())) {
            ++done;
            if (after_tick) after_tick();
        }
        const uint32_t t = now_ms();
        if (have_present_ && t - last_present_ms_ < target) sleep_ms(target - (t - last_present_ms_));
        last_present_ms_ = now_ms();
        have_present_ = true;
    }
    return done;
}

// --------------------------------------------------------------------------------------- commands
cmd::SubmitResult SimHost::submit(uint8_t player, cmd::EventData ev) {
    return pipeline_->submit(player, cmd::CommandSource::Player, std::move(ev));
}

cmd::RejectReason SimHost::execute_direct(uint8_t player, cmd::CommandSource source, const cmd::EventData& ev) {
    return pipeline_->execute_direct(frame_, player, source, ev);
}

void SimHost::record_to(cmd::CommandRecording* rec) {
    record_ = rec;
    if (rec) rec->seed = cfg_.seed;
    pipeline_->record_to(rec);
}

void SimHost::replay_from(const cmd::CommandRecording* rec) {
    if (!rec) {
        pipeline_->replay_from(nullptr);
        return;
    }
    cfg_.seed = static_cast<uint32_t>(rec->seed);
    cfg_.scheduler = rec->config;
    make_pipeline(rec->config);
    pipeline_->replay_from(rec);
}

uint32_t SimHost::wire_id(ObjectHandle h) const {
    const GameMode* t = modes_.top();
    if (!t || !t->primary().exists(h)) return cmd::kNoObject;
    return static_cast<uint32_t>(t->primary().id(h));
}

ObjectHandle SimHost::resolve(uint32_t wire) const {
    const GameMode* t = modes_.top();
    if (!t || wire == cmd::kNoObject || wire > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) return {};
    const ObjectHandle h = t->primary().find(static_cast<ObjectId>(wire));
    return t->primary().active(h) ? h : ObjectHandle{};  // refix t01: dying / removing objects (death clones) are not targets
}

std::vector<ObjectHandle> SimHost::owned_units(uint8_t player, const std::vector<uint32_t>& ids) const {
    std::vector<ObjectHandle> out;
    const GameMode* t = modes_.top();
    if (!t) return out;
    for (uint32_t id : ids) {
        const ObjectHandle h = resolve(id);
        if (!h || t->primary().owner(h) != static_cast<int32_t>(player)) continue;
        if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    }
    return out;
}

cmd::RejectReason SimHost::handle(uint64_t tick, const cmd::Command& c) {
    GameMode* top = modes_.top();
    GalacticMode* g = top && !top->tactical() ? static_cast<GalacticMode*>(top) : nullptr;
    // --- P host (owner: game host agent): port-only events (PortExtension 63) the game host consumes itself, in any mode ---
    if (port_hook_ && c.payload.type() == cmd::EventType::PortExtension) {
        const cmd::RejectReason pr = port_hook_(*this, c.header.player, c.payload);
        if (pr != cmd::RejectReason::Unhandled) {
            if (pr == cmd::RejectReason::None) ++applied_;
            else ++rejected_;
            return pr;
        }
    }
    // --- end P host ---
    cmd::RejectReason r = (g && g->sim()) ? g->sim()->handle_command(tick, c) : apply(c.header.player, c.payload);
    if (r == cmd::RejectReason::Unhandled && g && g->sim() && galactic_cmd_) r = galactic_cmd_(*this, c.header.player, c.payload);  // Int-final
    // --- P4 host (owner: game host agent): an original event no system handled (e.g. Withdrawl 49) goes to the port hook ---
    if (r == cmd::RejectReason::Unhandled && port_hook_ && c.payload.type() != cmd::EventType::PortExtension)
        r = port_hook_(*this, c.header.player, c.payload);
    // --- end P4 host ---
    if (r == cmd::RejectReason::None) ++applied_;
    else ++rejected_;
    return r;
}

void SimHost::station_replaced(ObjectId old_station, ObjectId new_station) {
    for (uint32_t p = 0; p < cfg_.players && p < kMaxPlayers; ++p) {
        std::vector<ObjectId>& sel = selection_[p];
        if (std::find(sel.begin(), sel.end(), old_station) != sel.end() &&
            std::find(sel.begin(), sel.end(), new_station) == sel.end())
            sel.push_back(new_station);
        for (std::vector<ObjectId>& g : groups_[p])
            if (std::find(g.begin(), g.end(), old_station) != g.end() && std::find(g.begin(), g.end(), new_station) == g.end())
                g.insert(std::lower_bound(g.begin(), g.end(), new_station), new_station);
    }
}

cmd::RejectReason SimHost::apply(uint8_t player, const cmd::EventData& ev) {
    using R = cmd::RejectReason;
    using T = cmd::EventType;
    GameMode* top = modes_.top();
    if (!top) return R::NotAllowed;
    if (player >= cfg_.players) return R::InvalidPlayer;
    objects::ObjectStore& store = top->primary();
    TacticalMode* tm = modes_.top_tactical();
    std::vector<ObjectId>& sel = selection_[player];
    auto selection_ids = [&sel] {
        std::vector<uint32_t> v;
        v.reserve(sel.size());
        for (ObjectId id : sel) v.push_back(static_cast<uint32_t>(id));
        return v;
    };
    auto order_all = [&](const std::vector<ObjectHandle>& units, UnitOrder o, bool need_locomotor) {
        uint32_t n = 0;
        for (ObjectHandle h : units) {
            if (need_locomotor && !tm->movable(h)) continue;
            tm->set_order(store.id(h), o);
            ++n;
        }
        return n;
    };
    // --- P host (owner: game host agent): human unit orders into the running battle (set_order_hook) ---
    if (tm && order_hook_) {
        const T t = ev.type();
        if (t == T::MoveToPosition || t == T::MoveToPositionFacing || t == T::MoveToRay || t == T::MoveToRayFacing || t == T::Facing ||
            t == T::MoveToObject || t == T::Attack || t == T::Escort || t == T::StopMovement || t == T::Retreat || t == T::Reinforce) {
            const R r = order_hook_(*this, player, ev, sel);
            if (r == R::None) story_order_feed(player, ev, sel);  // S4: STORY_COMMAND_UNIT / STORY_GUARD_UNIT
            if (r != R::Unhandled) return r;
        }
    }
    if (tm && ev.type() == T::ControlGroup) {  // control groups (sim_host.hpp control_group)
        std::vector<ObjectId>& grp = groups_[player][ev.u("group") % 10u];
        if (ev.u("assign") != 0) {
            if (ev.u("add") == 0) grp.clear();
            for (const ObjectId id : sel)
                if (std::find(grp.begin(), grp.end(), id) == grp.end()) grp.push_back(id);
            std::sort(grp.begin(), grp.end());
            groups_used_ = true;
            return R::None;
        }
        std::vector<uint32_t> wire;
        for (const ObjectId id : grp) wire.push_back(static_cast<uint32_t>(id));
        sel.clear();
        for (ObjectHandle h : owned_units(player, wire)) sel.push_back(store.id(h));
        grp = sel;  // dead / lost members leave the group
        return sel.empty() ? R::InvalidUnit : R::None;
    }
    // --- end P host ---
    switch (ev.type()) {
        case T::Select: {
            const auto& ids = ev.ids("ids");
            sel.clear();
            for (ObjectHandle h : owned_units(player, ids)) sel.push_back(store.id(h));
            return (!ids.empty() && sel.empty()) ? R::InvalidUnit : R::None;
        }
        case T::SelectAll: {
            // q6 (COMMANDS §2.4.1): replace the selection with the issuer's SELECTABLE objects in owner-list order, without
            // base structures, star bases, orbital structures, mines and objects with a parent; no category filter, no cap.
            static const objects::BehaviorTypeId kGroundStructure = objects::behavior_type_id("DUMMY_GROUND_STRUCTURE");
            static const objects::BehaviorTypeId kStarBase = objects::behavior_type_id("DUMMY_STAR_BASE");
            static const objects::BehaviorTypeId kOrbital = objects::behavior_type_id("DUMMY_ORBITAL_STRUCTURE");
            static const objects::BehaviorTypeId kMine = objects::behavior_type_id("PROXIMITY_MINE");
            sel.clear();
            std::vector<ObjectId> members;
            auto add = [&sel](ObjectId id) {
                if (std::find(sel.begin(), sel.end(), id) == sel.end()) sel.push_back(id);  // already selected: nothing
            };
            for (ObjectHandle h : store.owned_by(player)) {
                if (!store.active(h) || !store.has_behavior(h, objects::beh::Selectable)) continue;
                if (store.has_behavior(h, kGroundStructure) || store.has_behavior(h, kStarBase) || store.has_behavior(h, kOrbital) ||
                    store.has_behavior(h, kMine))
                    continue;
                if (store.exists(store.container(h))) continue;  // a parent (store link: contained / transported)
                members.clear();
                if (select_all_hook_ && !select_all_hook_(*this, player, h, members)) continue;
                add(store.id(h));
                for (const ObjectId m : members) add(m);  // a TEAM container: + its members
            }
            return R::None;
        }
        case T::MoveToPosition: {
            if (!tm) return R::NotAllowed;
            const float x = ev.f("x"), y = ev.f("y"), z = ev.f("z");
            const tactical::Aabb& b = tm->bounds();
            if (!finite(x) || !finite(y) || !finite(z) || x < b.min.x || x > b.max.x || y < b.min.y || y > b.max.y)
                return R::InvalidPosition;
            UnitOrder o;
            o.kind = OrderKind::Move;
            o.player = player;
            o.issued_frame = frame_;
            o.goal = {x, y, z};
            return order_all(owned_units(player, selection_ids()), o, true) ? R::None : R::InvalidUnit;
        }
        case T::MoveToObject: {
            if (!tm) return R::NotAllowed;
            const ObjectHandle target = resolve(ev.u("target"));
            if (!target) return R::InvalidTarget;
            const auto& movers = ev.ids("movers");
            UnitOrder o;
            o.kind = OrderKind::MoveToObject;
            o.player = player;
            o.issued_frame = frame_;
            o.target = store.id(target);
            return order_all(owned_units(player, movers.empty() ? selection_ids() : movers), o, true) ? R::None
                                                                                                       : R::InvalidUnit;
        }
        case T::Attack: {
            if (!tm) return R::NotAllowed;
            const ObjectHandle target = resolve(ev.u("target"));
            if (!target || store.owner(target) == static_cast<int32_t>(player)) return R::InvalidTarget;
            const auto& attackers = ev.ids("attackers");
            UnitOrder o;
            o.kind = OrderKind::Attack;
            o.player = player;
            o.issued_frame = frame_;
            o.target = store.id(target);
            return order_all(owned_units(player, attackers.empty() ? selection_ids() : attackers), o, false)
                       ? R::None
                       : R::InvalidUnit;
        }
        case T::StopMovement: {
            if (!tm) return R::NotAllowed;
            const auto units = owned_units(player, selection_ids());
            for (ObjectHandle h : units) tm->clear_order(store.id(h));
            return units.empty() ? R::InvalidUnit : R::None;
        }
        default: return (tm && tactical_cmd_) ? tactical_cmd_(*this, player, ev) : R::Unhandled;  // S10-int
    }
}

// --- S4 (owner: S4 agent): order-hook story feed (sim_host.hpp story_order_feed) ---
void SimHost::story_order_feed(uint8_t player, const cmd::EventData& ev, const std::vector<ObjectId>& selection) {
    using T = cmd::EventType;
    StorySession* s = story_.get();
    TacticalMode* tm = modes_.top_tactical();
    const T t = ev.type();
    if (!s || !tm || !s->battle_attached() || s->tactical_player() != static_cast<int32_t>(player)) return;
    if (t != T::MoveToPosition && t != T::MoveToPositionFacing && t != T::MoveToRay && t != T::MoveToRayFacing && t != T::Escort) return;
    const bool ray = t == T::MoveToRay || t == T::MoveToRayFacing;  // P1: the plane point b is the destination
    const objects::ObjectStore& st = tm->primary();
    std::vector<ObjectId> units;
    if (t == T::MoveToPositionFacing && !ev.ids("units").empty()) {
        for (const uint32_t w : ev.ids("units"))
            if (w != cmd::kNoObject && w <= 0x7FFFFFFFu) units.push_back(static_cast<ObjectId>(w));
    } else {
        units = selection;
    }
    ObjectId target = objects::kNoObjectId;
    if (t == T::Escort) {
        const ObjectHandle th = resolve(ev.u("target"));
        if (!th) return;
        target = st.id(th);
    }
    std::vector<ObjectId> done;
    for (const ObjectId id : units) {
        const ObjectHandle h = st.find(id);
        if (!st.active(h) || st.owner(h) != static_cast<int32_t>(player) || id == target) continue;
        if (std::find(done.begin(), done.end(), id) != done.end()) continue;
        done.push_back(id);
        TacticalStoryNotification n;
        n.object = id;
        if (t == T::Escort) {
            n.kind = TacticalStoryEvent::UnitGuarding;
            n.other = target;
        } else {
            n.kind = TacticalStoryEvent::UnitCommanded;
            n.has_position = true;
            const float z = ev.f(ray ? "bz" : "z");
            n.x = ev.f(ray ? "bx" : "x");
            n.y = ev.f(ray ? "by" : "y");
            n.z = finite(z) ? z : st.position(h).z;
        }
        s->notify_tactical(n);
    }
}
// --- end S4 ---

}  // namespace alamo::sim
