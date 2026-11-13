// alamo_game App: attract demo, movies, story steps, session / battle flow (split from game_main.cpp).
#include "game_app.hpp"

namespace alamo_game_app {

void App::show_menu_after_intro() {
    screen_ = Screen::Menu;
    open_main_menu();
}

// --- OI host: the main-menu attract demo (attract_mode.hpp, docs/re/MAIN_MENU_BACKGROUND.md) ---
// 0xc58e0: the main menu opens - with Main_Menu_Demo_Attract_Mode (or --attract) the demo starts now, or the running one
// gets its map-cycle check. Scripted runs start the demo from their own steps (their frame-driven runs stay machine
// independent and their sessions stay the scripted ones).
void App::open_main_menu() {
    ui_.front.show_main_menu();
    // GALAXY_MAP_PRESENTATION §5: the version line on the menu backdrop. [port] fixed text: the retail version number
    // (FoC 1.121) and the port's build tag, so screenshots stay machine independent.
    ui_.gui.set_version_text(U"V 1.121 alamo");
    audio_.music(MusicScene::MainMenu);
    const bool first = first_menu_;
    first_menu_ = false;
    if (!script_.empty()) return;
    const AttractController::Request r = attract_.menu_opened(now_s_, opt_.attract);
    if (r == AttractController::Request::None) return;
    if (!start_attract_map()) {
        attract_.stop();
        return;
    }
    if (first) fade_state_ = 1;
}

// Per frame (0x22c50): input (keys, buttons, wheel, mouse motion) refreshes the idle clock and never stops the demo;
// idle past Start_Timeout = the demo's tactical pause (step_sim); the map cycles after its delay once the user idles.
bool App::attract_step(const ui::InputFrame& raw) {
    const bool moved = raw.mouse_x != last_mouse_x_ || raw.mouse_y != last_mouse_y_;
    last_mouse_x_ = raw.mouse_x;
    last_mouse_y_ = raw.mouse_y;
    if (!script_.empty() || bench_active() || !attract_.armed()) return false;  // BM2a: no attract demo
    const bool input = raw.keys_pressed != ui::KeySet{} || raw.buttons_pressed != 0 || raw.wheel != 0.0f || moved;
    using D = AttractController::Dialogs;
    const D dialogs = ui_.gui.find("IDD_MAIN_MENU_DIALOG") ? D::MainMenu : ui_.gui.top() ? D::Other : D::None;
    if (attract_.update(now_s_, input, dialogs) == AttractController::Request::None) return false;
    if (start_attract_map()) return true;
    stop_attract();
    return false;
}

// The demo session (0x22f10): a space skirmish with no human - the players of faction indices 0 and 6 (FoC: Rebel /
// BasicRebel and Underworld / AI_Player_Underworld; GameSession picks them) - on the controller's map. EaW .rec entries (Gabriel 2026-10-28): the recording is not replayed;
// its header names the map ([port] scan, attract_rec_map) and the same live demo runs there; no map path -> the entry is
// skipped with a warning. [port] fallback: the first space skirmish map when the entry does not load as a space map.
bool App::start_attract_map() {
    SessionSpec s;
    s.kind = SessionKind::Skirmish;
    s.attract = true;
    s.seed = opt_.seed + static_cast<uint32_t>(attract_.starts());
    s.battle_max_frames = static_cast<uint32_t>(std::max(attract_.cycle_delay(), 60.0) * 30.0) + 300u;  // the cycle reloads first
    // modc t20: no slot factions - the demo's sides are faction indices (GameSession, 0x22f10); empty slots take the data's
    // default_skirmish_faction for the scaffolding galaxy.
    std::vector<std::string> maps;
    std::string entry = attract_.map();
    if (is_attract_recording(entry)) {
        std::vector<uint8_t> bytes;
        const std::string names[] = {"Recordings\\" + entry, "Data\\Recordings\\" + entry, entry};
        bool read = false;
        for (const std::string& n : names)
            if (!read && data_.files->read(n, bytes)) read = true;
        const std::string m = read ? attract_rec_map(bytes) : std::string();
        if (m.empty()) std::fprintf(stderr, "[warn] attract: %s: %s - entry skipped\n", entry.c_str(), read ? "no map path in the header" : "not found");
        entry = m;
    }
    if (!entry.empty()) maps.push_back(entry);
    const std::vector<std::string> space = skirmish_space_maps(data_);
    if (!space.empty() && (maps.empty() || maps.front() != space.front())) maps.push_back(space.front());
    for (const std::string& m : maps) {
        s.skirmish_map = m;
        if (start_session(s)) {
            ++attract_runs_;
            demo_shot_t0_ = -1.0;
            audio_flags();
            std::printf("attract: demo on %s (start %" PRIu64 ")\n", m.c_str(), attract_.starts());
            return true;
        }
    }
    return false;
}

// 0x22bf0: the demo game ends; the dialogs, the menu music and screen_ are the caller's.
void App::stop_attract() {
    attract_.stop();
    if (demo_session()) end_session();
    fade_state_ = 0;
    audio_flags();
}

// [port] The original films the demo with the in-game cinematic camera ("interesting object" shots, combat shots first;
// shot logic untraced, MAIN_MENU_BACKGROUND.md §3.1 step 8; a code-driven camera, no camera data in the map or the demo's
// Lua). Stand-in [M]: 8 s shots, each on the next live combat unit (ascending id, cycling); the camera keeps a side view of
// that hull (attract_mode.hpp demo_shot_yaw_deg: playtest 3 t14, hulls were seen nose-on), pitch 35, the tactical zoom stays.
// Presentation only (wall time).
void App::demo_camera() {
    const sim::TacticalBattle* b = battle();
    if (!b || !bview_.active()) return;
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;
    const render::SnapshotObject* target = nullptr;
    const DemoCameraTuning tune;
    for (const render::SnapshotObject& o : objs)
        if (o.id == demo_target_ && b->combat().has_unit(o.id)) target = &o;
    if (!target || demo_shot_t0_ < 0.0 || now_s_ - demo_shot_t0_ > tune.shot_seconds) {
        std::vector<const render::SnapshotObject*> units;
        // [M] only hulls that are drawn (model, not fogged / contained / dying) and fly (no stations).
        constexpr uint8_t kNotShown = render::kSnapDying | render::kSnapBackground | render::kSnapFogHidden | render::kSnapCorpse | render::kSnapContained;
        for (const render::SnapshotObject& o : objs)
            if (o.owner >= 0 && b->combat().has_unit(o.id) && (o.flags & render::kSnapDrawable) != 0 && (o.flags & kNotShown) == 0 &&
                b->movement().view(o.id).max_speed > 0.0f)
                units.push_back(&o);
        if (units.empty())
            for (const render::SnapshotObject& o : objs)
                if (o.owner >= 0 && b->combat().has_unit(o.id)) units.push_back(&o);
        if (units.empty()) return;
        target = units[demo_shots_++ % units.size()];
        demo_target_ = target->id;
        demo_shot_t0_ = now_s_;
        demo_stats_.run = 0;
        if (demo_side_view_) demo_yaw_ = demo_side_yaw_deg(target->facing.x, target->facing.y, bview_.camera().yaw_deg());  // a cut
    }
    ui::TacticalCameraController& cam = bview_.camera();
    cam.jump_to({target->position.x, target->position.y, target->position.z});
    if (demo_side_view_) {
        demo_yaw_ = demo_shot_yaw_deg(demo_yaw_, target->facing.x, target->facing.y, static_cast<float>(now_s_ - demo_shot_t0_), dt_, tune);
        cam.set_yaw_deg(demo_yaw_);
        cam.set_pitch_deg(tune.pitch_deg);
    } else {
        cam.rotate(4.0f * dt_, 0.0f);
    }
    cam.step(dt_);
    // Statistics of what the camera now shows (view direction from the pose, so the yaw convention is checked too).
    const ui::CameraParams cp = cam.params(16.0f / 9.0f);
    const float vx = cp.target.x - cp.eye.x, vy = cp.target.y - cp.eye.y, vl = std::sqrt(vx * vx + vy * vy);
    if (vl < 1e-3f) return;
    const float yaw_now = std::atan2(vx, vy) * (180.0f / 3.14159265f);
    ++demo_stats_.frames;
    if (demo_view_alignment(yaw_now, target->facing.x, target->facing.y) > 0.866f) {
        ++demo_stats_.target_nose_on;
        demo_stats_.longest_run = std::max(demo_stats_.longest_run, ++demo_stats_.run);
    } else {
        demo_stats_.run = 0;
    }
    for (const render::SnapshotObject& o : objs) {
        if (o.owner < 0 || !b->combat().has_unit(o.id)) continue;
        const float dx = o.position.x - cp.target.x, dy = o.position.y - cp.target.y;
        if (dx * dx + dy * dy > cam.distance() * cam.distance()) continue;  // roughly what is on screen
        ++demo_stats_.hulls;
        if (demo_view_alignment(yaw_now, o.facing.x, o.facing.y) > 0.866f) ++demo_stats_.hulls_nose_on;
    }
}

bool App::menu_dialog_open() const {
    static constexpr const char* kMenuDialogs[] = {
        "IDD_GAME_OPTIONS_DIALOG",       "IDD_IN_GAME_OPTIONS_DIALOG",   "IDD_AUDIO_OPTIONS_DIALOG",   "IDD_VIDEO_OPTIONS_DIALOG",
        "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDD_GAMEPLAY_OPTIONS_DIALOG", "IDD_SAVE_GAME_DIALOG",      "IDD_LOAD_GAME_DIALOG",
        "IDD_DIALOG_BOX_QUIT_CONFIRM",   ui::kPortControlsDialog};
    for (const char* n : kMenuDialogs)
        if (ui_.gui.find(n)) return true;
    for (const std::string& n : ui_.gui.open_names())
        if (n.rfind("IDD_KEYBOARD_OPTIONS_DIALOG", 0) == 0) return true;
    return false;
}

// Audio game-state flags (GameAudioHost::set_flags): modal = a modal GUI dialog has the focus; gui_only = [port] the
// in-game pause menu (or its option dialogs) over a session; attract = the attract demo runs.
void App::audio_flags() {
    const bool modal = ui_.gui.has_focus();
    audio_.set_flags(modal, modal && paused_ && session_ != nullptr, attract_.running());
}

// The Lua presentation's camera is cinematic (listener at the camera, AUDIO.md §1.2).
bool App::presentation_cinematic() const {
#ifdef ALAMO_SIM_HAVE_STORY
    sim::StorySession* story = session_ ? session_->host().story() : nullptr;  // unique_ptr: non-const pointee
    const lua::PresentationLog* log = story ? story->presentation() : nullptr;
    return log && log->state().camera == lua::CameraMode::Cinematic;
#else
    return false;
#endif
}
// --- end OI host ---

// ---- Bink movies (game_movies.hpp): the intro / logo movies before the main menu (skippable with Esc / Space / click) ----
bool App::next_movie() {
    std::string err;
    if (!movies_available(&err)) {
        std::fprintf(stderr, "movies skipped: %s\n", err.c_str());
        return false;
    }
    while (movie_index_ < opt_.movies.size()) {
        const std::string& name = opt_.movies[movie_index_++];
        if (full_movie_.open(data_, audio_, name, false, &err)) {
            std::printf("movie: %s (%s)\n", name.c_str(), full_movie_.audio_clock() ? "audio clock" : "wall clock");
            return true;
        }
        std::fprintf(stderr, "movie %s: %s\n", name.c_str(), err.c_str());
    }
    return false;
}

void App::stop_movies() {
    full_movie_.stop(audio_);
    advisor_movie_.stop(audio_);
    gpu_.hud.clear_image_page(kMoviePage);
    gpu_.hud.clear_image_page(kAdvisorPage);
}

void App::movie_step() {
    // Movies.xml Cannot_Skip: the skip keys do nothing for that movie. UI.md §4.2: while a movie plays only Esc / Space skip
    // (UI5 t07: a mouse click does not).
    const bool skip = !full_movie_.cannot_skip() && (input_.key_pressed(ui::vk::kEscape) || input_.key_pressed(ui::vk::kSpace));
    const bool playing = full_movie_.update(dt_, gpu_.hud, kMoviePage);
    if (playing && !skip) return;
    full_movie_.stop(audio_);
    if (next_movie()) return;
    gpu_.hud.clear_image_page(kMoviePage);
    show_menu_after_intro();
}

// Lua presentation ops of the story VMs (lua::PresentationLog, read between ticks): Play_Bink_Movie -> the full-screen
// slot, or the HUD advisor slot for *_LOOP* movies [port]; Stop_Bink_Movie stops both; the audio ops go to GameAudio
// (apply_presentation_audio). The script's movie block finishes on sim frames (movie_frames), not on playback.
void App::lua_presentation() {
#ifdef ALAMO_SIM_HAVE_STORY
    sim::StorySession* story = session_ ? session_->host().story() : nullptr;
    lua::PresentationLog* log = story ? story->presentation() : nullptr;
    if (!log) return;
    for (const lua::PresentationEvent& e : log->events()) {
        if (e.seq < lua_seq_) continue;
        lua_seq_ = e.seq + 1;
        if (e.op == lua::PresentationOp::PlayBinkMovie && !e.args.empty()) {
            const std::string& name = e.args.front().text;
            const std::string up = ascii_upper(name);
            std::string err;
            MovieSlot& slot = up.find("_LOOP") != std::string::npos ? advisor_movie_ : full_movie_;
            if (!slot.open(data_, audio_, name, &slot == &advisor_movie_, &err)) std::fprintf(stderr, "Lua movie %s: %s\n", name.c_str(), err.c_str());
        } else if (e.op == lua::PresentationOp::StopBinkMovie) {
            stop_movies();
        } else if (e.op == lua::PresentationOp::ActivateRetryDialog) {  // mission lost: IDD_RETRY_DIALOG (ui1 t05)
            // [port] its Retry ("Load autosave") restarts the mission: the port keeps no mission-start autosave.
            if (screens_.show_retry()) ++retry_boxes_;
        }
        if (session_->battle()) {  // story t13: the battle's letterbox overlay and the P11 hand-over (story_ui.hpp)
            using Op = lua::PresentationOp;
            if (e.op == Op::LetterBoxIn || e.op == Op::LetterBoxOn) linked_ui_.script_letterbox(true);
            else if (e.op == Op::LetterBoxOut || e.op == Op::LetterBoxOff) linked_ui_.script_letterbox(false);
            else if (e.op == Op::LockControls || e.op == Op::StartCinematicMode || e.op == Op::EndCinematicMode ||
                     e.op == Op::StartCinematicCamera || e.op == Op::EndCinematicCamera)
                linked_ui_.script_controls();
        }
    }
    audio_.apply_presentation(log->events(), &lua_audio_seq_);
#ifdef ALAMO_UI_HAVE_LUA
    // HUD_GAPS §5.2: Lua GUI_Component_* (mods only; vanilla ships no GUI scripts) override the HUD's own state. The log
    // only grows within a story: the calls past the ones already applied go to the current screen's HUD.
    if (log->events().size() != gui_log_events_) {
        gui_log_events_ = log->events().size();
        std::vector<ui::GuiScriptCall> calls;
        ui::gui_script_calls(*log, calls);
        if (calls.size() < gui_calls_seen_) gui_calls_seen_ = 0;
        if (calls.size() > gui_calls_seen_) {
            ui::HudButtons& buttons = screen_ == Screen::Battle ? ui_.tactical_hud.buttons() : ui_.galactic_hud.buttons();
            std::vector<std::string> errs;
            gui_calls_applied_ += ui::apply_gui_scripts(std::span<const ui::GuiScriptCall>(calls).subspan(gui_calls_seen_), ui_.components,
                                                        buttons, &ui_.text, now_s_, &errs);
            for (const std::string& er : errs) std::fprintf(stderr, "Lua GUI: %s\n", er.c_str());
            gui_calls_seen_ = calls.size();
        }
    }
#endif
#endif
}
// L1/S4 wave 1 (story_ui.hpp): this frame's UI notifications -> GameSession::notify_story_ui (a command; port_event hands
// it to StorySession::notify_ui at the next command phase, so a replay sees the same story). Galactic screen: the
// observed state (selection - a selected fleet with its member units [inferred: Param1 names unit types] -, selected
// planet, camera zoom, open corruption panel) and the HUD clicks (galactic_input). Battle screen with a tactical plot set
// (tactical_story): the selection (battle-store ids) and the tactical HUD clicks (battle_input) as `tactical`
// notifications, which the sim routes to the tactical plot set; speech ends raised then are `tactical` too (the sim also
// routes them to the galactic sets). Movie ends, the battle-end screen's close and a lost mission are raised in any mode
// (to the galactic plot sets). Nothing is sent without a story session.
// L1/S4 wave 1: the media of story rewards fired since the last frame by the local player's plot sets (galactic sets in set
// order, then the tactical set; firing order within a set): START_MOVIE (Reward_Param1) -> the full-screen movie slot,
// whose end raises STORY_MOVIE_DONE with the movie name (a movie that cannot be opened ends at once [port]: MOVIE_DONE has
// no timeout); MULTIMEDIA (Reward_Param8) -> GameAudioHost::speech, whose end (AudioSystem::on_speech_done) raises
// STORY_SPEECH_DONE (a speech that cannot play keeps the 60 s timeout); MULTIMEDIA Reward_Param9 *_Loop -> the advisor slot
// (Movie_galactic / Movie_tactical) until STOP_COMMANDBAR_MOVIE (FIXPASS_GALACTIC §4.2). Presentation only: the sim sees the
// ends through the recorded story UI commands. [port] the reward's other presentation (MULTIMEDIA text, START_MOVIE music
// halt) is not shown here.
void App::story_reward_step() {
#ifdef ALAMO_SIM_HAVE_STORY
    sim::StorySession* story = session_ && !demo_session() ? session_->host().story() : nullptr;
    if (!story) {
        reward_cursor_.clear();
        reward_tac_model_ = nullptr;
        reward_tac_cursor_ = 0;
        return;
    }
    std::vector<StoryMediaCue> cues;
    auto scan = [&cues](const lua::StoryModel& m, size_t& cursor) {
        const std::vector<lua::StoryFired>& fired = m.fired();
        if (cursor > fired.size()) cursor = fired.size();
        for (; cursor < fired.size(); ++cursor) {
            const lua::StoryFired& f = fired[cursor];
            if (f.reward_type != "START_MOVIE" && f.reward_type != "MULTIMEDIA" && f.reward_type != "STOP_COMMANDBAR_MOVIE") continue;
            std::vector<std::string> params;
            for (const lua::StoryParam& p : f.reward_params) params.push_back(p.text);
            story_reward_media(f.reward_type, params, cues);
        }
    };
    reward_cursor_.resize(story->plot_set_count(), 0);
    for (size_t set = 0; set < story->plot_set_count(); ++set) {
        if (story->plot_set_player(set) == local_player()) scan(story->model(set), reward_cursor_[set]);
        else reward_cursor_[set] = story->model(set).fired().size();
    }
    const lua::StoryModel* tac = story->tactical_model();
    if (tac != reward_tac_model_ || battles_seen_ != reward_tac_battle_) {
        // [inferred] a talking head left by a tactical plot ends with its battle (its STOP_COMMANDBAR_MOVIE can no longer fire).
        if (reward_tac_model_ && !story_movie_) {
            advisor_movie_.stop(audio_);
            gpu_.hud.clear_image_page(kAdvisorPage);
        }
        reward_tac_model_ = tac;
        reward_tac_battle_ = battles_seen_;
        reward_tac_cursor_ = 0;
    }
    if (tac && story->tactical_player() == local_player()) scan(*tac, reward_tac_cursor_);
    for (const StoryMediaCue& c : cues) {
        if (c.kind == StoryMediaCue::Kind::Speech) {
            audio_.speech(c.name);
            ++reward_speeches_;
            continue;
        }
        if (c.kind == StoryMediaCue::Kind::AdvisorMovie || c.kind == StoryMediaCue::Kind::AdvisorStop) {
            if (story_movie_) continue;  // a random-plot offer's dialog holds the slot
            std::string err;
            if (c.kind == StoryMediaCue::Kind::AdvisorStop) advisor_movie_.stop(audio_);
            else if (!advisor_movie_.open(data_, audio_, c.name, true, &err)) std::fprintf(stderr, "advisor movie %s: %s\n", c.name.c_str(), err.c_str());
            if (!advisor_movie_.playing()) gpu_.hud.clear_image_page(kAdvisorPage);
            continue;
        }
        ++reward_movies_;
        std::string err;
        if (full_movie_.playing()) {  // [inferred] one full-screen movie at a time: the playing one ends first
            story_ui_.movie_done(full_movie_.name());
            full_movie_.stop(audio_);
        }
        if (!full_movie_.open(data_, audio_, c.name, false, &err)) {
            std::fprintf(stderr, "story movie %s: %s\n", c.name.c_str(), err.c_str());
            story_ui_.movie_done(c.name);
        }
    }
#endif
}

bool App::tactical_story() const {
#ifdef ALAMO_SIM_HAVE_STORY
    if (!session_ || !session_->battle()) return false;
    const sim::StorySession* story = session_->host().story();
    return story && story->tactical_model() != nullptr;
#else
    return false;
#endif
}

void App::story_ui_step() {
    story_ui_.set_battle(tactical_story());
    for (const std::string& e : audio_.take_speech_done()) story_ui_.speech_done(e);  // STORY_SPEECH_DONE
    if (!session_ || demo_session()) {
        (void)story_ui_.take();
        return;
    }
    if (screen_ == Screen::Battle && story_ui_.battle()) story_ui_.observe_battle(orders_.selection());  // battle-store ids
    if (screen_ == Screen::Galactic) {
        StoryUiView v;
        const galactic::GalacticSim* g = session_->galaxy();
        for (const uint32_t id : orders_.selection()) {
            v.selection.push_back(id);
            if (!g) continue;
            const objects::ObjectHandle h = g->store().find(static_cast<objects::ObjectId>(id));
            if (g->store().alive(h) && g->fleet_of(h) != galactic::kNoFleet)
                for (const objects::ObjectHandle m : g->contents_of(h)) v.selection.push_back(static_cast<uint32_t>(g->store().id(m)));
        }
        v.planet = selected_planet_;
        v.zoomed = gview_.camera().zoomed();
        v.zoom_planet = zoom_planet_;
        if (const SpecialDialog* d = session_->special_dialog(); d && d->kind == galactic::kSpecialCorruption && corruption_.open())
            v.corruption_planet = d->planet_id;
        story_ui_.observe(v);
    }
    for (const sim::StoryUiNotification& n : story_ui_.take()) {
        if (bench_active()) bench_.story_pending.push_back(n);  // BM2a: sent before the next tick (bench_step_sim)
        else if (session_->notify_story_ui(n)) ++story_ui_sent_;
    }
}

void App::shutdown() {
    set_os_cursor_hidden(false);
    restore_display_mode();  // [port] the monitor mode a video option switched
    stop_movies();
    gpu_.ring.wait_idle();
    ctx_.wait_idle();
    bscen_.gpu.shutdown();  // bm2: the benchmark's GPU timer (bench_finish normally shuts it down first)
    if (bview_.active()) bview_.stop(gpu_);
    session_.reset();
    gview_.destroy();
    ui_.shutdown(gpu_);
    panel_drawer_.destroy(gpu_.renderer);
    targets_.destroy(gpu_.alloc);
    gpu_.shutdown();
    sc_.shutdown();
}

std::string App::local_faction() const {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return {};
    const int32_t p = local_player();
    if (p < 0 || static_cast<size_t>(p) >= g->players().size()) return {};
    const uint32_t f = g->players()[static_cast<size_t>(p)].faction;
    return f < g->rules().factions.size() ? g->rules().factions[f].name : std::string();
}

// ------------------------------------------------------------------------------------------------ session flow
bool App::start_session(const SessionSpec& spec) {
    if (!spec.attract) attract_.stop();  // 0x22bf0: a real game starts (end_session below ends the demo session)
    end_session();
    recording_ = commands::CommandRecording{};
    session_ = std::make_unique<GameSession>(data_, *js_, spec);
    std::vector<std::string> diag;
    if (!session_->start(&diag, nullptr, &recording_)) {
        for (const std::string& d : diag) std::fprintf(stderr, "session: %s\n", d.c_str());
        session_.reset();
        return false;
    }
    loaded_save_.clear();
    session_started(diag);
    credits_capped_ = false;
    // ui4 t08 [port]: a new campaign (not a load, a skirmish or the attract demo) plays the faction's
    // SFXEvent_GUI_Start_Campaign once it has started.
    if (spec.kind == SessionKind::Campaign && !spec.attract && galaxy()) {
        audio_.faction_sfx("SFXEvent_GUI_Start_Campaign");
        ++announcements_;
    }
    return true;
}

// The presentation side of a session that just started or was loaded (session_ is set).
void App::session_started(const std::vector<std::string>& diag) {
    const SessionSpec& spec = session_->spec();
    apply_gameplay_options();  // ui1 t02: the session start reset the game speed indices (MAIN_LOOP.md §1.5)
    holo_.reset();
    story_cursor_ = 0;
    story_ui_.reset();  // L1/S4 wave 1: a new session's UI state
    reward_cursor_.clear();  // ... and its story reward media from the start
    reward_tac_model_ = nullptr;
    reward_tac_cursor_ = 0;
    zoom_planet_ = 0;
    battle_plots_.clear();
    tally_ = {};
    campaign_over_ = choice_open_ = auto_pending_ = battle_end_auto_ = false;
    battle_view_pending_ = false;
    load_intro_frames_ = 0;
    battle_place_.clear();
    prompt_ = {};
    ar_log_seen_ = 0;
    holocron_chapters_.clear();
    holocron_flash_ = story_summary_open_ = false;
    summary_entry_ = 0;
    summary_rows_.clear();
    story_scripts_.clear();  // the new session's plots ask for their dialog scripts again (sync_story_scripts)
    story_loads_ = {};
    screen_lines_.clear();
    orders_ = ui::OrderTranslator{};
    orders_.set_selection({});
    last_submit_frame_ = 0;
    hotkeys_.set_paused(false);  // OI host: a new session starts unpaused (the Pause hotkey's first press pauses)
    if (spec.attract) {  // OI host: the demo runs behind the main menu - the menu, its faction and backdrop stay
        screen_ = Screen::Menu;
        std::printf("session: attract demo %s\n", spec.skirmish_map.c_str());
        for (const std::string& d : diag) std::printf("  session: %s\n", d.c_str());
        return;
    }
    gui_calls_seen_ = gui_log_events_ = 0;  // a new story log
    hp_flash_.clear();
    hover_ship_ = 0;
    hud_hardpoints_.clear();
    hud_hp_refs_.clear();
    ui_.set_faction(local_faction(), data_.rules.constants.good_side_name, data_.rules.constants.evil_side_name);
    ui_.gui.close_all();
    ui_.gui.set_backdrop({});  // the front end's backdrop (FrontEnd::show_main_menu) is not drawn in game
    ui_.front.set_in_game(true);
    if (const galactic::GalacticSim* g = session_->galaxy()) gview_.attach(*g, local_player());
    gview_.capture(session_->host());
    if (spec.kind == SessionKind::Campaign) {
        screen_ = Screen::Galactic;
        audio_.music(MusicScene::Galactic, local_faction());
    } else {
        screen_ = Screen::SessionLoad;  // the hidden skirmish galaxy runs until the battle is pushed
        open_battle_load(kInvalidPlanet, !spec.skirmish.land);  // QA t11: on screen before the push's blocking load
    }
    std::printf("session: %s %s, local player %d (%s), galactic AI %s, story %s\n", spec.kind == SessionKind::Campaign ? "campaign" : "skirmish",
                spec.kind == SessionKind::Campaign ? spec.campaign.c_str() : spec.skirmish_map.c_str(), local_player(), local_faction().c_str(),
                session_->galactic_ai_running() ? "on" : "off (stand-in)", session_->story_running() ? "on" : "off");
    if (session_->loaded())
        std::printf("  session: loaded from %s at tick %" PRIu64 "%s\n", loaded_save_.c_str(), session_->host().frame(),
                    session_->pause_requested() ? " (paused)" : "");
    for (const std::string& d : diag) std::printf("  session: %s\n", d.c_str());
    // N1: a save taken during a battle comes back in it: the battle opens now (its load screen, then Begin), before any
    // tick - post_tick would only notice the pushed battle after a first tick on the galactic map / hidden skirmish galaxy.
    if (session_->in_battle()) begin_battle();
}

void App::end_session() {
    if (bview_.active()) bview_.stop(gpu_);
    if (ui_.load.is_open()) ui_.load.close();
    battle_view_pending_ = false;
    load_intro_frames_ = 0;
    session_.reset();
    ui_.front.set_in_game(false);
}

void App::begin_battle() {
    const sim::TacticalBattle* b = session_->battle();
    ++battles_seen_;
    reinforce_unit_ = 0;  // no drop point pending from an earlier battle
    bombing_targeting_ = false;  // nor a bombing-run pick
    autofire_seen_.clear();  // ui1 t02: the default autofire is sent for this battle's objects
    sw_weapon_ = objects::kNoObjectId;  // ui1 t04: nor a space special-weapon aim
    bombing_blob_.reset();
    targeting_.cancel();  // nor a targeted ability
    reinforce_blob_.reset();  // ui3: nor a drop preview / beacon pick; tether and interface keys reset per battle [port]
    beacon_pick_ = false;
    camera_tether_ = false;
    cycle_unit_ = 0;
    // L1/S4 wave 1: the battle's tactical plots (loaded by CampaignBattles at the tactical push) for STORY_MISSION_LOST;
    // battle-screen story notifications go to that plot set (story_ui_step / battle_input).
    const sim::StorySession* story = session_->host().story();
    battle_plots_ = story ? story->tactical_plots_file() : std::string();
    if (demo_session()) {  // OI host: no load screen; the world is drawn under the main menu from the first frame
        start_battle_view();
        // The skirmish map loader hands every battle the session's map (game_session.cpp): name that one, not the
        // conflict request's label (the skirmish galaxy planet's own map, _Space_Planet_Kuat_01.ted - never loaded).
        std::printf("battle %" PRIu64 ": attract demo, %s map %s at tick %" PRIu64 "\n", battles_seen_, bview_.space() ? "space" : "land",
                    session_->spec().skirmish_map.c_str(), session_->host().frame());
        return;
    }
    const sim::TacticalMode* tm = session_->host().modes().top_tactical();
    const bool space = tm && tm->kind() == objects::ModeKind::Space;
    const PlanetIndex planet = b ? b->request().info.planet : kInvalidPlanet;
    battle_place_ = battle_place_name(planet);
    const bool story_mission = b && b->request().linked;  // UI5 t07: the Mission load dialogs
    // story t13 (BT §2.9.1 P10 / P11): a story-linked battle opens with the record's presentation (applied by the start,
    // CampaignBattles::linked_presentation); any other battle with nothing held.
    {
        const sim::CampaignBattles* cb = session_->battles();
        const bool linked = b && cb && b->request().linked;
        const sim::CampaignBattles::LinkedPresentation none;
        const sim::CampaignBattles::LinkedPresentation& lp = linked ? cb->linked_presentation() : none;
        linked_ui_.begin(lp.letterbox, lp.presentation_b, b ? b->frame() : 0);
        if (linked)
            std::printf("battle %" PRIu64 ": story link, letterbox %s, input block %s\n", battles_seen_, lp.letterbox ? "on" : "off",
                        lp.presentation_b ? "on" : "off");
    }
    // QA t11: the dialog first, the blocking BattleView::start only after it has been presented (a Fight answer or a
    // skirmish start opened it even earlier, before the tactical push's own load).
    if (ui_.load.is_open()) load_intro_frames_ = 1;
    else if (!open_battle_load(planet, space, story_mission)) notes_.push_back("load screen dialog missing");
    if (ui_.load.is_open()) {
        screen_ = Screen::BattleLoad;
        battle_view_pending_ = true;
    } else {
        start_battle_view();
        screen_ = Screen::Battle;
        audio_.music(MusicScene::Battle, local_faction(), space);
    }
    std::printf("battle %" PRIu64 ": %s map %s at tick %" PRIu64 "\n", battles_seen_, space ? "space" : "land",
                b ? b->request().map.c_str() : "-", session_->host().frame());
}

void App::start_battle_view() {
    battle_view_pending_ = false;
    std::string err;
    bview_.set_blocking_models(!script_.empty() || bench_fixed());  // BM2a: fixed frames draw every model on its first frame  // scripted runs: a new model draws on its first frame (screenshots)
    if (!bview_.start(gpu_, data_, *session_, targets_.width, targets_.height, &err)) {
        std::fprintf(stderr, "battle view: %s\n", err.c_str());
        notes_.push_back("battle view failed: " + err);
        if (ui_.load.is_open()) ui_.load.close();  // its progress would never fill
        if (screen_ == Screen::BattleLoad) {
            screen_ = Screen::Battle;
            audio_.music(MusicScene::Battle, local_faction(), bview_.space());
        }
    }
    gpu_.renderer.reset_taa_history();  // a new map: nothing to reproject from
    bview_.camera().set_fov_override(tactical_fov_set_ ? camera_.tactical_fov_deg : 0.0f);  // options: tactical FOV (0 = the data's)
    bview_.camera().set_tuning(option_tuning(bview_.camera().tuning()));  // P1_CAMERA_INPUT.md §3.1 options
    bview_.capture(*session_);
    audio_.battle_reset();
    cinematic_reset();  // refixf t05
    radar_fog_uploads_ = ~uint64_t{0};
    radar_events_.clear();
    radar_feed_.reset();
    radar_fields_.clear();
    radar_fields_built_ = radar_fields_dirty_ = false;
}

void App::end_battle() {
    if (session_ && session_->battles())
        for (const std::string& l : session_->battles()->log()) std::printf("  %s\n", l.c_str());
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    if (session_ && session_->battles() && !session_->battles()->ai_log().empty())
        std::printf("  tactical AI: %s\n", session_->battles()->ai_log().back().c_str());
#endif
    const BattleViewStats& bs = bview_.stats();
    std::printf("battle view: %u objects (%u drawn, %u fog-hidden), %u effects, %u corpse draws, %" PRIu64 " particles spawned, fog %s, %" PRIu64
                " scorch decals, %" PRIu64 " muzzle flashes shown\n",
                bs.objects, bs.drawn, bs.fog_hidden, bs.effects, bs.corpses, bs.particles_spawned, bs.fog_drawn ? "drawn" : (bs.fog ? "present" : "none"),
                bs.decals, bs.muzzles);
#ifdef ALAMO_SIM_HAVE_ANIM
    if (session_ && session_->animation()) {
        const sim::SimAnimStats& as = session_->animation()->stats();
        std::printf("animation: %u models (%u with clips), %u rows created, %u muzzle flashes, %u flinches, %u death clips\n", as.models,
                    as.models_with_clips, as.created, as.muzzle_flashes, as.flinches, as.death_clips);
    }
#endif
    cinematic_reset();  // refixf t05: a shot still running when the battle ends (the hold is released before frame 765)
    linked_ui_.reset();  // story t13
    bview_.stop(gpu_);
    if (ui_.load.is_open()) ui_.load.close();
    orders_.set_selection({});
    queues_.clear();
    if (demo_session()) return;  // OI host: no battle-end screen; the attract controller moves on (screen_ stays Menu)
    // story t13 (BT §2.9.6): the battle end took the dialog-skip flag 0x140b305ed of a LINK_TACTICAL record: the dialog
    // callback 0x87770 returns without the battle-summary dialog. [M] It also sets the galactic mode's battle-end-closed flag
    // (+0x188, STORY.md §4.1 step 6): GENERIC battle_end_closed now, as a closed screen would raise it.
    const sim::CampaignBattles* cb = session_ ? session_->battles() : nullptr;
    if (cb && cb->linked_presentation().last_battle_skipped_dialog && session_->spec().kind != SessionKind::Skirmish) {
        show_battle_end(false);
        story_ui_.generic("battle_end_closed");
        ++linked_dialog_skips_;
        std::printf("battle end: story link, no battle-summary dialog\n");
        screen_ = Screen::Galactic;
        gview_.capture(session_->host());
        audio_.music(MusicScene::Galactic, local_faction());
        return;
    }
    show_battle_end();  // IDD_BATTLE_END_DIALOG; ticks wait for Continue (presentation-only hold)
    if (session_ && session_->spec().kind == SessionKind::Skirmish) {
        screen_ = Screen::Menu;  // the battle-end screen's Continue goes back to the main menu
        ui_.front.set_in_game(false);
        if (!battle_end_open_) {
            skirmish_ended_ = true;
            if (opt_.smoke.empty()) session_.reset();
            open_main_menu();
        }
        return;
    }
    screen_ = Screen::Galactic;
    gview_.capture(session_->host());
    audio_.music(MusicScene::Galactic, local_faction());
}

// --- P4 host: FoC galactic special systems (docs/re/P4_SIM_SYSTEMS.md) on the presentation side ---

}  // namespace alamo_game_app
