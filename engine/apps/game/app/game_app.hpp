// alamo_game (P host), the App class and the helpers its sources share. game_main.cpp has init / run / main; the other
// app_*.cpp files hold the App methods by section: session flow, input, screens, HUD frames, render + frame, script.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/ascii.hpp"  // ascii_iequals / ascii_upper: exact ASCII, slash-exact
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/game/attract_mode.hpp"
#include "alamo/game/battle_fx.hpp"
#include "alamo/game/battle_summary.hpp"
#include "alamo/game/benchmark.hpp"
#include "alamo/game/benchmark_save.hpp"
#include "alamo/game/benchmark_scenario.hpp"
#include "alamo/game/game_audio_host.hpp"
#include "alamo/game/game_controls.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/game/hud_feeds.hpp"
#include "alamo/game/mod_check.hpp"
#include "alamo/game/mod_picker.hpp"
#include "alamo/game/radar_feed.hpp"
#include "alamo/game/save_games.hpp"
#include "alamo/game/story_ui.hpp"
#include "alamo/game/world_pick.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/render/galactic/galactic_session.hpp"
#include "alamo/render/png.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/victory.hpp"     // Int-verify: campaign end
#include "alamo/sim/campaign_battles.hpp"  // Int-final: corruption command
#include "alamo/ui/gui_info_screens.hpp"
#include "alamo/ui/gui_screens.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/hud_markers.hpp"
#include "alamo/ui/hud_panels.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "alamo/ui/hud_sim_bridge.hpp"
#ifdef ALAMO_UI_HAVE_LUA
#include "alamo/ui/gui_script_lua.hpp"
#endif
#ifdef ALAMO_UI_HAVE_ABILITIES
#include "alamo/ui/hud_abilities.hpp"
#endif
#include "alamo/ui/mouse_cursor.hpp"
#include "alamo/ui/orders.hpp"
#include "alamo/ui/render_bridge.hpp"
#include "alamo/ui/targeting.hpp"
#include "alamo/ui/window_input.hpp"
#include "frame_overlay.hpp"
#include "game_display.hpp"
#include "game_info.hpp"
#include "game_render.hpp"
#include "game_ui.hpp"
#include "game_movies.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/presentation.hpp"
#include "alamo/lua/story_model.hpp"
#include "alamo/sim/story_session.hpp"
#endif

namespace alamo_game_app {


using namespace alamo;
using namespace alamo::game;
using Clock = std::chrono::steady_clock;

inline std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

inline double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
inline uint64_t final_crc(const CrcLog& log) { return log.records.empty() ? 0 : log.records.back().combined; }

// The planet row of a galactic planet object id (kInvalidPlanet when none).
inline PlanetIndex planet_index(const galactic::GalacticSim& g, uint32_t planet_id) {
    for (size_t i = 0; i < g.planets().size(); ++i)
        if (g.store().exists(g.planets()[i].object) && static_cast<uint32_t>(g.store().id(g.planets()[i].object)) == planet_id)
            return static_cast<PlanetIndex>(i);
    return kInvalidPlanet;
}

// WM_CHAR text for GUI edit boxes: rhi::Window::take_text (printable characters only; Backspace / Enter / Tab / Esc reach
// the GUI as key presses).
inline std::u32string printable(std::u32string s) {
    std::u32string out;
    for (const char32_t c : s)
        if (c >= 0x20 && c != 0x7F) out.push_back(c);
    return out;
}
struct Options {
    std::string game, cache_dir, png_dir, smoke;
    std::vector<std::string> game_args;
    std::vector<std::string> argv;  // modc t08: argv[1..] as given (the mod picker relaunches with these, its mod args replaced)
    std::vector<std::string> movies;  // --movie NAME: Bink movies played before the main menu (Esc / click skips)
    bool intro = true;                // default movies: the LucasArts / Petroglyph logos (--no-intro: none)
    bool attract = false;             // --attract: start the main-menu attract demo at once (OI host)
    uint32_t width = 1280, height = 720;
    bool hidden = false;
    AudioBackendKind audio = AudioBackendKind::Device;
    uint32_t seed = 1;
    int32_t workers = -1;
    long frames = -1;
    uint32_t battle_max_frames = 27000;
    rhi::PresentMode mode = rhi::PresentMode::Fifo;
    int32_t frame_cap = 0;  // --frame-cap N|display (ui::kFrameCapChoices semantics: 0 none, -1 the display's refresh rate)
    bool mode_set = false, frame_cap_set = false;  // --mailbox / --immediate / --frame-cap given: they win over the video options file
    // [port] --video-options FILE: the video options file to read (live play reads and writes the default one,
    // game_display.hpp default_video_profile_path; smoke / hidden runs read only this one and write none).
    std::string video_options;
    // N1 / t09: --saves-dir DIR = where the Save / Load dialogs keep saves (default: %LOCALAPPDATA%\alamo\Saves; smoke
    // runs: <png-dir>\saves).
    std::string saves_dir;
    // OPT1 t07: --frame-overlay (F11 toggles it in live play); --profile [--profile-out FILE] [--profile-budget MS]: the
    // tick profiler + per-phase frame times for the whole run, printed at exit (frame_overlay.hpp).
    bool frame_overlay = false, profile = false;
    std::string profile_out;
    double profile_budget = 1000.0 / 30.0;
    // BM2a: --benchmark realtime|fixed and the --bench-* flags (benchmark.hpp; app_benchmark.cpp). size_set: --width /
    // --height given (they win over the preset's window size).
    BenchmarkOptions bench;
    BenchNewSave bench_new;  // BM3: --bench-new-save / --bench-maps (a render-free mode that never opens a window)
    bool size_set = false;
};

inline int usage() {
    std::fprintf(stderr,
                 "usage: alamo_game [--game DIR] [--width W] [--height H] [--hidden] [--audio device|null|off] [--seed S]\n"
                 "                  [--workers N] [--frames N] [--battle-max-frames N] [--cache-dir DIR] [--mailbox|--immediate]\n"
                 "                  [--frame-cap N|display] [--video-options FILE] [--saves-dir DIR]\n"
                 "                  [--movie NAME]... [--no-intro] [--attract] [--smoke skirmish|land|campaign|saveload --png-dir DIR] [MODPATH=..]\n"
                 "                  [--frame-overlay] [--profile] [--profile-out FILE] [--profile-budget MS] [FALLBACKPATH=..]\n"
                 "       alamo_game --benchmark realtime|fixed --bench-save FILE [--bench-preset NAME] [--bench-ticks N]\n"
                 "                  [--bench-frames-per-tick K] [--bench-settle N] [--bench-max-unsettled N] [--bench-report FILE]\n"
                 "                  [--bench-crc-out FILE] [--bench-png-dir DIR --bench-png-every N] [--bench-no-verify]\n"
                 "                  presets: bm-low, bm-medium, bm-high (default), bm-ultra, bm-smoke\n"
                 "       alamo_game --bench-scenario S.abench [--bench-* flags]     a BM1/BM2 scenario (host game); flags win over the file\n"
                 "       alamo_game --bench-make S.abench --bench-save FILE [...]   run from the save, then write the scenario S\n"
                 "       game data: --game DIR or ALAMO_FOC_DATA (the corruption folder)\n");
    return 2;
}

enum class Screen : uint8_t { Menu, SessionLoad, Galactic, BattleLoad, Battle, Quit, Movie };
constexpr uint16_t kVkF11 = 0x7A;  // OPT1 t07: frame-time overlay toggle (not in the default hotkey table)
constexpr int32_t kLoadIntroMaxFrames = 30;  // QA t11: the most frames the sim waits for the load screen's textures
constexpr uint32_t kBattleEndHoldFrames = 150;  // [port] Playtest QA D1: 5 s between the victory and the summary (the last break-up plays)
constexpr uint64_t kLoadIntroMaxTicks = 30;  // sim ticks a Fight answer may take to push its battle
inline const char* screen_name(Screen s) {
    switch (s) {
        case Screen::Movie: return "movie";
        case Screen::Menu: return "menu";
        case Screen::SessionLoad: return "session-load";
        case Screen::Galactic: return "galactic";
        case Screen::BattleLoad: return "battle-load";
        case Screen::Battle: return "battle";
        case Screen::Quit: return "quit";
    }
    return "?";
}

class App;
// One scripted step: fills the frame's input (or leaves it idle) and returns true when done. Steps run in order.
struct ScriptStep {
    std::string name;
    std::function<bool(App&, ui::InputFrame&)> run;
    uint32_t timeout_frames = 3600;
};

class App {
public:
    explicit App(Options o) : opt_(std::move(o)) {}
    int run();

    // ---- used by the script ----
    Options opt_;
    GameData data_;
    std::unique_ptr<jobs::JobSystem> js_;
    rhi::Window win_;
    rhi::Context ctx_;
    rhi::Swapchain sc_;
    GpuCore gpu_;
    FrameTargets targets_;
    GameUi ui_;
    GameAudioHost audio_;
    GalacticView gview_;
    BattleView bview_;
    // refixf t05: the in-game cinematic camera (TSW_PLANET_DESTROY.md §11; battle_fx.hpp, app_cinematic.cpp): the TSW
    // firing shot started by the Fire cue, its shake trackers (the state-9 explosion), the letterbox, the saved RTS camera.
    // Presentation only: driven by the tactical frame count, overrides the rendered battle camera and the listener, hides
    // the HUD and the cursor, swallows mouse input; never written into the sim.
    struct Cinematic {
        TswFiringShot shot;
        CameraShake shake;
        Letterbox letterbox;
        UnsyncedRandom rng;  // the unsynced generator (NoiseOffset draws), reseeded from the wall clock at init
        objects::ObjectId tsw = objects::kNoObjectId;
        SavedCamera<ui::TacticalCameraController> saved_camera;  // +0x2c8: the mode camera saved once at cinematic mode on
        int32_t clock = 0;  // shot clock of the frame being drawn
        bool enabled = true;                  // GameConstants In_Game_Cinematics
        float letterbox_height = 0.085f;      // GameConstants Camera_FX_Manager_Letterbox_Height
        uint32_t shots = 0, skipped = 0, completed = 0, shakes = 0;  // diagnostics
    } cine_;
    void tsw_shot_begin(const TswCue& c, const sim::TacticalBattle& b, const objects::ObjectStore& st);
    void tsw_shake(const TswCue& c, const sim::TacticalBattle& b);
    void tsw_shot_end();      // End_Cinematic_Mode + letterbox out
    void cinematic_reset();   // a battle view starts / stops: everything off at once
    void cinematic_step();    // per frame after the sim: shot clock, timer end, letterbox slide
    void cinematic_input(ui::InputFrame& raw);  // §11.2 input skip rules (before the HUD / world routing)
    bool cinematic_camera(ui::CameraParams& cp);  // the shot's camera over the RTS params; false when no shot runs
    bool cinematic_hides_hud() const { return cine_.shot.active(); }
    // The cursor is hidden unless the attract demo runs; [port] a GUI dialog with the focus (pause menu) shows it again.
    bool cinematic_hides_cursor() const { return cine_.shot.active() && !demo_session() && !ui_.gui.has_focus(); }
    void draw_letterbox(ui::HudDrawList& dl, const ui::HudCanvas& canvas, uint32_t w, uint32_t h) const;
    // story t13 (story_ui.hpp LinkedBattleOverlay): a story-linked battle's P10 letterbox / P11 input block and the story
    // scripts' letterbox, drawn by draw_letterbox with the cinematic's bars; begun by begin_battle, stepped by
    // cinematic_step, cleared by cinematic_reset.
    LinkedBattleOverlay linked_ui_;
    void linked_ui_step();
    std::unique_ptr<GameSession> session_;
    commands::CommandRecording recording_;
    // Bink movies: the full-screen slot (intro / logos, Lua Play_Bink_Movie) and the HUD advisor loop slot.
    MovieSlot full_movie_, advisor_movie_;
    size_t movie_index_ = 0;
    static constexpr uint16_t kMoviePage = ui::kHudPageCount - 1;    // [port] the last image page: full-screen movie
    static constexpr uint16_t kAdvisorPage = ui::kHudPageCount - 2;  // [port] the one before: advisor loop
    uint32_t lua_seq_ = 0, lua_audio_seq_ = 0;  // presentation-log cursors (movie ops / audio ops)
    bool next_movie();
    void stop_movies();
    bool advisor_loop_shown() const {  // the advisor slot shows on the HUD, not in a story dialog's portrait
        return advisor_movie_.has_frame() && advisor_movie_.playing() && !(story_movie_ && story_page_ >= 0);
    }
    void movie_step();
    void lua_presentation();
    Screen screen_ = Screen::Menu;
    ui::InputState input_;
    ui::InputRouter router_;
    ui::RoutedInput routed_;
    ui::OrderTranslator orders_;
    // refixf t08: the tactical world pick's model / sphere tests (HUD_GAPS §2.2, world_pick.hpp), refreshed by
    // world_pick_tests() for the current battle; Mouse_Collide_Override_Sphere_Radius per type.
    ui::WorldPickTests pick_tests_;
    std::vector<float> pick_radii_;
    // Targeted-ability mode (TargetingController, UI.md §4.1): while it is active the router gives it the presses, so it
    // reads the RAW frame through its own InputState (the world's input_ never sees those clicks).
    ui::TargetingController targeting_;
    ui::InputState target_input_;
    std::vector<ui::GuiEvent> gui_events_;
    std::vector<ScriptStep> script_;
    size_t script_step_ = 0;
    uint32_t step_frames_ = 0;
    bool script_failed_ = false;
    std::string shot_path_;   // set: the next rendered frame is read back into this PNG
    bool shot_done_ = false;
    bool hold_sim_ = false;   // presentation-only pause (screenshots settle; never changes what the sim computes)
    bool hold_release_ = false;  // hold_sim_ ends before the NEXT script step runs (that step keeps its frame's ticks)
    bool script_hold_ = false;  // the same for a whole stretch of script steps (step_shot leaves it alone)
    uint32_t story_dialog_frames_ = 0;
    bool paused_ = false;     // pause menu
    // N1 / t09: saved games (save_games.hpp; the Save / Load dialogs, MenuAction SaveGame / LoadGame / DeleteSave).
    std::string saves_dir_;
    std::string loaded_save_;  // the file the current session was loaded from ("" = a new game)
    uint64_t saves_written_ = 0, saves_loaded_ = 0;
    uint64_t last_submit_frame_ = 0;
    uint64_t frames_ = 0;
    double now_s_ = 0.0;
    float dt_ = 0.0f;
    uint32_t last_tick_ms_ = 0;
    uint32_t prev_tick_ms_ = 0;  // the tick before last_tick_ms_ (render interpolation period)
    Clock::time_point t0_ = Clock::now();
    // [port: Gabriel] video: the swap mode V-sync uses, the frame rate cap, the window's monitor refresh rate.
    ui::VsyncMode vsync_mode_ = ui::VsyncMode::Fifo;
    FrameLimiter limiter_;
    uint32_t refresh_hz_ = 0;
    uint64_t refresh_checked_frame_ = 0;
    // [port: Gabriel] advanced video: ambient occlusion and anisotropic filtering (anti-aliasing, TAA, is the profile's
    // anti_alias strength). The detail levels and the soft shadow / heat / bloom checks reach the renderer in apply_detail_options (game_display.hpp, t10 / t11). world_view_: the last
    // frame's world (0 none, 1 galactic, 2 battle); a change restarts the TAA history.
    ui::AdvancedVideoSettings adv_video_;
    int world_view_ = 0;
    // [port] the video options file (game_display.hpp VideoProfile): read at start, written on every Video / Advanced
    // video Apply in live play. detail_options_: the advanced detail options reach the renderer (live play, or a run
    // given --video-options; other smoke runs keep the renderer's defaults).
    VideoProfile video_profile_;
    std::string video_profile_path_;
    bool have_video_profile_ = false, detail_options_ = false;
    void save_video_options() {
        if (live_input() && !save_video_profile(video_profile_path_, video_profile_)) std::printf("video options: %s not written\n", video_profile_path_.c_str());
    }
    render::DrawStats draw_stats_;
    // OPT1 t07: the frame-time overlay and this frame's phase times (wall clock, presentation only).
    FrameOverlay overlay_;
    FrameTimes ft_;
    Clock::time_point frame_t0_{};
    bool smoke_ok_ = true;
    std::vector<std::string> notes_;
    uint64_t battles_seen_ = 0;
    // p4ui t06: the skirmish map catalog (UI.md section 9), built once when the menus are set up (null before).
    std::shared_ptr<const game::SkirmishMapCatalog> skirmish_catalog_;
    // ---- P4 screens, panels, hotkeys, cursor, radar (second hosting pass) ----
    ui::GameScreens screens_;
    ui::InfoScreens info_;
    std::vector<render::FactionColor> faction_colors_;  // Factions.xml <Color> (FactionFiles.xml order)
    ui::HotkeyDispatcher hotkeys_;
    ui::CursorTable cursor_table_;
    ui::MouseCursor cursor_;
    bool cursor_ok_ = false;
    bool hardware_mouse_ = false;
    std::vector<ui::HudObjective> hud_objectives_;  // tactical objectives box (built per frame)  // video options IDC_HARDWARE_MOUSE: the OS arrow instead of the drawn cursor
    ui::RadarSettings radar_settings_;
    ui::RadarImage radar_;
    ui::RadarEvents radar_events_;
    uint64_t radar_fog_uploads_ = ~uint64_t{0};
    RadarFeed radar_feed_;  // radar_feed.hpp: death / capture-point pings, story blips (host-side, read-only)
    std::vector<ui::RadarField> radar_fields_;  // space battle's asteroid fields (painted into the radar picture)
    bool radar_fields_built_ = false, radar_fields_dirty_ = false;
    ui::EncyclopediaPanel ency_;
    // HUD_GAPS §2.2: the encyclopedia config (GameConstants) with the profile's IDC_DELAY_TOOLTIPS; the type of each recently
    // hovered object (the pop-up keeps drawing its shown_object() while fading out after the hover is lost).
    ui::TooltipConfig ency_cfg_;
    std::vector<std::pair<uint32_t, objects::TypeIndex>> ency_types_;
    // Gameplay options (IDD gameplay dialog). ui1 t02: kept between runs with the audio volumes, camera settings and key
    // bindings in game_options.txt (game_display.hpp OptionsProfile; live play only, app_options.cpp).
    ui::GameplaySettings gameplay_;
    ui::AudioVolumes audio_opts_;
    std::string options_path_;
    std::vector<uint32_t> autofire_seen_;  // ui1 t02: local objects already given their default autofire (sorted)
    void load_game_options();
    void save_game_options();
    void apply_gameplay_options();  // alternate mouse, game speeds
    void queue_default_autofire(std::vector<commands::EventData>& cmds);
    bool map_shell_open_ = false;  // HUD_GAPS §4.2: map_shell shown (advanced planet map)
    bool planet_map_open_ = false;  // GALACTIC_DIALOGS §4: the FoC planet map (organiser's advanced_planetary button)
    ui::HeroPanel hero_panel_;
    std::vector<std::pair<uint32_t, objects::TypeIndex>> hero_types_;  // hero bar entries: object -> type (hover pop-up)
    bool story_controls_locked() const;  // the story's Lock_Controls state (FIXPASS_GALACTIC §2.2 gate)
    ui::BlackMarketPanel market_;
    ui::SlicePanel slice_;  // OI host: the slice dialog (kind 2) on the market shell
    ui::SabotagePanel sabotage_;
    ui::CorruptionPanel corruption_;
    ui::HackPanel hack_;
    ui::ShellSet panel_shells_;
    ui::ShellDrawer panel_drawer_;
    bool panel_shells_ok_ = false;
    std::vector<std::pair<uint32_t, uint32_t>> drag_moves_;  // galactic drag-to-move this frame (fleet, planet)
    // GRC 1.1: the galactic HUD's planet list (object ids, the HUD drag's over_planet index) and the move orders to
    // unrevealed planets the host dropped; SFXEvent_Planet_Revealed plays.
    std::vector<uint32_t> hud_planet_ids_;
    uint64_t hidden_moves_dropped_ = 0, reveal_sfx_ = 0;
    // HUD_GAPS feeds (presentation only): the tactical HUD units of the last build, the hovered ship (reticle owner kept
    // while the cursor is on its reticle, §1.1), its hard-point reticles (+ ship / index / kind of each), the reticle
    // texture table (GameConstants), the image-page resolver for the stand-alone reticle DDS, the flash start per
    // (ship, hard point) of the last attack / targeted-ability order on it (wall seconds, §1.3), the galactic hero roster,
    // the consumed Lua GUI_* calls (mods) and the HUD hit debug dump (ALAMO_HUD_HIT_DEBUG).
    struct HardPointRef {
        uint32_t ship = 0, index = 0;
        uint8_t kind = 0;
    };
    std::vector<ui::HudUnit> hud_units_;
    uint32_t hover_ship_ = 0;
    std::vector<ui::HudHardPoint> hud_hardpoints_;
    std::vector<ui::HudBuildOption> build_opts_;  // tactical buy bar of the selected builder (TacticalHudFrame::build_options)
    std::vector<HardPointRef> hud_hp_refs_;
    ui::ReticleTable reticles_;
    ui::GuiImageResolver hud_images_;
    std::map<uint64_t, double> hp_flash_;
    std::vector<ui::HeroEntry> galactic_heroes_;
    size_t gui_calls_seen_ = 0, gui_log_events_ = 0;
    uint64_t gui_calls_applied_ = 0, reticle_orders_ = 0, reticle_frames_ = 0;
    uint64_t buy_bar_behind_camera_ = 0;  // q6: frames the selected builder's buy bar was hidden (builder behind the camera)
    bool hud_hit_debug_ = false;
    // ALAMO_HUD_HIT_DEBUG=x,y: probe that fixed pixel instead of the cursor (smoke runs) and list the visible shells.
    int32_t hud_hit_px_ = -1, hud_hit_py_ = -1;
    int32_t hud_hit_last_ = -2;
    void hud_hit_dump(const char* where, int32_t component, int32_t px, int32_t py);
    uint64_t refused_ = 0;                                   // commands the session refused (submit)
    uint32_t last_fleet_sel_ = 0;  // galactic: the fleet selection of the last frame (organiser planet)
    uint32_t selected_planet_ = 0;        // galactic planet selection (object id, 0 = none)
    bool fleet_drag_active_ = false;      // galactic HUD fleet drag of the last frame (drag / drop sounds)
    bool battle_end_open_ = false;        // battle-end screen shown: ticks held until Continue (presentation only)
    std::u32string battle_place_;         // the running battle's localized place (the battle-end screen's title line)
    int32_t load_intro_frames_ = 0;       // QA t11: frames left before the blocking load runs (0 = none; presentation only)
    uint32_t load_intro_shown_ = 0;       // frames presented since the load screen opened
    uint64_t load_open_tick_ = 0;         // sim tick at which the load screen opened ahead of the tactical push
    bool battle_view_pending_ = false;    // the load screen is up; BattleView::start runs once it has been presented
    bool skirmish_ended_ = false;
    SessionSpec pending_skirmish_;        // skirmish waiting for the buy-units screen
    ui::BuyUnitsState buy_;
    std::map<std::string, uint32_t> bought_;  // type -> count (skirmish purchases)
    struct AutoUnit {
        int32_t owner;
        objects::TypeIndex type;
        objects::ObjectHandle h;
    };
    std::vector<AutoUnit> auto_units_;  // the auto-resolve's forces when it started (losses = the ones gone after)
    std::vector<ui::HudOrderQueue> queues_;
    std::vector<ui::HudWaypointLine> waypoints_;  // movement path lines of the selected units (tactical HUD)
    uint64_t pause_toggles_ = 0;
    ui::CameraSettings camera_;         // FOVs / queue modifier from the options (camera and controls dialog)
    bool tactical_fov_set_ = false;     // the player changed the tactical FOV (else TacticalCameras.xml's applies)
    std::vector<DisplayMode> display_modes_;  // the display's modes (primary display, game_display.hpp)
    std::vector<DisplayMode> shown_modes_;    // UI5 t01: the video options' resolution list (video_list_modes; Widescreen)
    bool windowed_ = true;                    // UI5 t01: Options > Video > Windowed (false: full screen)
    uint64_t video_applies_ = 0;
    bool menu_dialog_open() const;  // the in-game menu or one of its option / save / load / quit dialogs is open
    uint32_t reinforce_unit_ = 0;       // reinforcement chosen on the pad, waiting for its drop point (0 = none)
    // cu9g t03 (HUD_GAPS §9.4 - §9.6): the land bombing-run targeting state (HUD +0x20b2): toggled by the b_special_weapon
    // click and the super-weapon hotkey; the world's next left click on a valid point sends command 28.
    bool bombing_targeting_ = false;
    std::optional<render::JamBlob> bombing_blob_;  // the green / red terrain blob under the cursor while targeting
    uint64_t bombing_commands_ = 0, bombing_cancels_ = 0, bombing_invalid_clicks_ = 0;
    bool bombing_run_ready() const;  // 0x312570's `ready`: controls not locked and the local run available
    void bombing_toggle();           // 0x2e7950 (cancel = 0): start (Select_Target SFX) or cancel (Cancelled SFX)
    void bombing_end(bool cancel_sfx);
    // P1 move acknowledgement (P1_DOUBLE_CLICK_MOVE.md §2.3): GameConstants values by ui::AckEffect / ui::AckRadar index.
    std::array<std::string, 4> ack_fx_{};
    std::array<std::string, 3> ack_radar_{};
    float ack_scale_land_ = 1.0f, ack_scale_space_ = 5.0f;  // GUI_Move_Acknowledge_Scale_Land / _Space
    uint64_t move_acks_ = 0, move_ack_kills_ = 0, move_acks_drawn_ = 0;
    void move_ack(const ui::MoveAck& a);
    // P1_CAMERA_INPUT.md §3.1 options -> CameraTuning (live play only; smoke / replay runs keep `t`).
    bool live_input() const { return opt_.smoke.empty() && !opt_.hidden && !opt_.bench.active(); }
    ui::CameraTuning option_tuning(ui::CameraTuning t) const;
    void apply_camera_tuning();
    // ency_cfg_ (GameConstants + IDC_DELAY_TOOLTIPS) to the encyclopedia, the galactic / tactical HUDs and the shell panels.
    void apply_tooltip_config();
    uint64_t reinforce_drops_ = 0;
    // Information screens fed from the sim (game_info.hpp): holocron history, story cursor, campaign tallies.
    Holocron holo_;
    size_t story_cursor_ = 0;
    bool summary_completed_ = false;
    BattleTally tally_;
    bool campaign_over_ = false;
    galactic::VictoryRules victory_rules_;  // Int-verify: the campaign's victory conditions (start_session)
    galactic::EliminationLatch elimination_latch_;  // q6 [port]: no elimination before the player has owned something
    bool choice_open_ = false;
    // GALACTIC_DIALOGS §1 battle prompt: the banner (pending_battle_shell / story_pending_battle_shell) fed through
    // GalacticHudFrame::battle while choice_open_; its clicks are HudAction BattleBegin / BattleAutoResolve.
    struct BattlePrompt {
        PlanetIndex planet = kInvalidPlanet;
        bool land = false;
        bool story_pending = false;   // §1.4 single-button shell (story lock): no Auto-Resolve
        bool linked = false;          // story t13: a LINK_TACTICAL record's P13 banner (Begin = GameSession::begin_linked)
        std::u32string title;         // "<planet>: SPACE / PLANETARY BATTLE IMMINENT!" (log; the ui composes its own)
        std::u32string planet_name;
        BattleForceSummary left, right;  // graph_left = the local player's force, graph_right = the enemy's
    } prompt_;
    // §3.1: the local player's Auto-Resolve choice waiting for its conflict-log entry; the log cursor for result lines.
    bool auto_pending_ = false;
    PlanetIndex auto_planet_ = kInvalidPlanet;
    size_t ar_log_seen_ = 0;
    bool battle_end_auto_ = false;    // the open battle-end screen follows an auto-resolve
    // §2 mission holocron. The list itself is sim state (sim::StorySession::holocron, GALACTIC_DIALOGS §2.8); here the
    // presented adds (Story_Dialog events, oldest first; presentation log only), the loaded story dialog scripts and the
    // summary's selection (a record id, 0 = the first row) and rows (record ids); the b_story_arc_g flash.
    struct HolocronChapter {
        std::string dialog, tag;
        int32_t chapter = -1;
        uint64_t id = 0;
    };
    std::vector<HolocronChapter> holocron_chapters_;
    StoryDialogCache story_scripts_;
    StoryScriptLoads story_loads_;
    uint64_t summary_entry_ = 0;
    std::vector<uint64_t> summary_rows_;
    bool holocron_flash_ = false;     // b_story_arc_g flashes until the summary is opened
    bool story_summary_open_ = false; // IDD_STORY_SUMMARY_DIALOG open: ticks held (the summary's galactic pause, §2.2)
    // Screen-text lines (§2.1 transmission line, white; §3.1 auto-resolve result, red) [port: printed and kept until a ui
    // screen-text widget takes them].
    struct ScreenLine {
        std::u32string text;
        ui::Rgba8 color;
        double at = 0.0;
    };
    std::vector<ScreenLine> screen_lines_;
    std::vector<ui::GalacticScreenText> screen_text_;  // this frame's feed (GalacticHudFrame::screen_text points here)
    uint64_t screen_line_count_ = 0, holocron_updates_ = 0, prompts_story_ = 0;
    uint64_t linked_banners_ = 0, linked_dialog_skips_ = 0;  // story t13
    void open_battle_prompt(const galactic::GalacticSim& g, const galactic::ConflictInfo& c);
    void open_linked_banner(const galactic::GalacticSim& g, const sim::CampaignBattles::LinkedRecord& r);  // story t13
    std::string representative_icon(std::span<const objects::ObjectHandle> units, bool hero_filter = false) const;
    void answer_battle_prompt(bool auto_resolve);
    void auto_resolve_result(const galactic::GalacticSim& g, const galactic::ConflictLogEntry& e);
    void add_holocron_chapter(const StoryDialogMeta& meta, size_t lines);
    void screen_line(std::u32string text, ui::Rgba8 color);
    std::u32string txt(std::string_view id, std::u32string_view fallback) const;
    std::u32string faction_label(int32_t player) const;
    uint64_t choices_ = 0, auto_resolves_ = 0, story_dialogs_ = 0, ency_shown_ = 0, retry_boxes_ = 0, story_cues_ = 0;
    uint64_t tactical_auto_resolves_ = 0, auto_results_ = 0;
    bool story_movie_ = false;  // the advisor slot plays a story dialog's MOVIE
    // L1/S4 wave 1 (story_ui.hpp): the local player's UI notifications for the story (StorySession::notify_ui through the
    // command stream); zoom_planet_ = the planet of the last zoom_to_planet; battle_plots_ = the running battle's tactical
    // plots file (the STORY_MISSION_LOST name); story_ui_sent_ = notifications submitted.
    StoryUiFeed story_ui_;
    uint32_t zoom_planet_ = 0;
    std::string battle_plots_;
    uint64_t story_ui_sent_ = 0;
    void story_ui_step();
    bool tactical_story() const;  // a battle runs with a tactical plot set (battle-screen notifications go there)
    // Story reward media (story_reward_media): fired-log cursors of the galactic plot sets (by set index) and of the
    // tactical plot set (reset when the tactical model or the battle count changes); movies / speeches started.
    std::vector<size_t> reward_cursor_;
    const void* reward_tac_model_ = nullptr;
    uint64_t reward_tac_battle_ = 0;
    size_t reward_tac_cursor_ = 0;
    uint64_t reward_movies_ = 0, reward_speeches_ = 0;
    void story_reward_step();
    int32_t story_page_ = -1;   // ui ask 11: the GUI image page the story movie fills (the dialog portrait), -1 none
    void galactic_screens();
    void open_holocron(ui::InfoPage page);
    void open_story_summary();
    void show_campaign_end(bool victory);
    void build_encyclopedia(ui::HudDrawList& dl, const ui::HudCanvas& canvas);
    // P4 host: the FoC galactic panels follow GameSession::special_dialog() (opened by a SpecialDialog event of the local
    // player); panel_preview_ = a presentation-only black-market preview of the smoke (no sim dialog, clicks ignored).
    uint32_t panel_kind() const;
    uint32_t panel_preview_ = 0;
    uint32_t preview_planet_ = 0;
    uint64_t special_sfx_ = 0, special_msgs_ = 0, activations_ = 0;
    bool activate_special_here(uint32_t hero_id);
    std::vector<objects::ObjectHandle> special_units(const galactic::GalacticSim& g, PlanetIndex pi) const;
    // OI host: main-menu attract demo (attract_mode.hpp), audio flags, fast forward.
    AttractController attract_;
    int32_t last_mouse_x_ = 0, last_mouse_y_ = 0;
    uint64_t attract_runs_ = 0, fast_toggles_ = 0;
    bool attract_step(const ui::InputFrame& raw);  // idle clock, idle pause, map cycle (wall time); true: a new map started
    bool start_attract_map();
    void stop_attract();    // a real game starts / credits / exit: the demo session ends (the dialogs stay)
    void open_main_menu();  // IDD_MAIN_MENU_DIALOG + menu music; starts / cycles the attract demo (0xc58e0)
    void show_menu_after_intro();
    void demo_camera();     // [port] stand-in for the in-game "interesting object" cinematic camera
    // First main-menu open (MAIN_MENU_BACKGROUND.md §2 step 6): a black full-screen rect, opaque until the demo battle has
    // ticked 4 frames, then fading out over 2000 ms. 0 none, 1 waiting for the demo, 2 fading.
    int fade_state_ = 0;
    double fade_t0_ = 0.0;
    bool first_menu_ = true;
    double demo_shot_t0_ = -1.0;  // [port] demo camera: current shot start (wall s), shot counter, followed object
    uint32_t demo_shots_ = 0;
    objects::ObjectId demo_target_ = 0;
    // Playtest 3 t14: the stand-in camera keeps a side view of the followed hull (attract_mode.hpp, demo_shot_yaw_deg).
    // demo_stats_ counts, per rendered demo frame, how often the followed hull / the hulls near the look-at point are seen
    // nose-on or tail-on (|cos| of view direction vs facing > 0.866 = within 30 degrees) and the longest run of such frames of
    // the followed hull; the skirmish smoke prints and checks them. Presentation only.
    bool demo_side_view_ = true;
    float demo_yaw_ = 0.0f;
    struct DemoCamStats {
        uint32_t frames = 0, target_nose_on = 0, run = 0, longest_run = 0, hulls = 0, hulls_nose_on = 0;
    } demo_stats_;
    // OI host: the attract demo session runs behind the main menu (screen_ stays Menu; its battle world is drawn under the
    // menu dialogs without the tactical HUD).
    bool demo_session() const { return session_ != nullptr && session_->spec().attract; }
    void audio_flags();
    bool presentation_cinematic() const;
    void special_feedback();
    std::vector<ui::HudPanel*> panels() { return {&hero_panel_, &market_, &slice_, &sabotage_, &corruption_, &hack_}; }
    void handle_screen_actions(const std::vector<ui::ScreenAction>& acts);
    void handle_hud_actions(const std::vector<ui::HudAction>& acts);
    // `voice`: per cmds entry, 0 = no unit voice response (the double-click order, UiOrder::voice); missing entries = 1.
    void route_commands(std::vector<commands::EventData>& cmds, std::span<const uint8_t> voice = {});
    void toggle_pause();
    void open_buy_units(const SessionSpec& spec);
    // A land skirmish map? Its catalog kind (mini 01) when the map is cataloged, else the old name rule ("_LAND" in the name).
    bool skirmish_map_is_land(const std::string& map) const;
    // Planet type shown for battle planet row `p`: in a skirmish the map's planet (skirmish_map_planet_type, [port]),
    // else / when the map names none the row's own type.
    uint32_t display_planet_type(const galactic::GalacticSim& g, PlanetIndex p) const;
    void update_display_rate(bool force);
    void set_present_mode(rhi::PresentMode m);
    float render_alpha();
    void show_battle_end(bool dialog = true);  // story t13: dialog false = the bookkeeping only (BT §2.9.6 skip)
    int32_t galactic_faction_icon(const galactic::GalacticSim& g, int32_t player) const;
    ui::Rgba8 faction_rgba(const galactic::GalacticSim& g, int32_t player, bool text, ui::Rgba8 fallback) const;
    void planet_icons(const galactic::GalacticSim& g, PlanetIndex row, ui::GalacticPlanetInfo& out) const;
    ui::PlanetInfoData planet_info_data(const galactic::GalacticSim& g, PlanetIndex row) const;
    void show_auto_resolve_end(const galactic::ConflictLogEntry& e);  // ui ask 10: the battle-end screen after an auto-resolve
    void build_panels(std::vector<ui::HudDrawList>& lists);
    void update_cursor(ui::HudDrawList& dl);
    // UI5 t02: with Hardware Mouse on, the current pointer's frame becomes the Windows cursor (same art, hot spot and
    // context cursors as the drawn one); off: the arrow class cursor (the drawn cursor hides it).
    void sync_os_cursor();
    std::map<std::string, void*> hw_cursors_;  // frame texture -> HCURSOR (null: could not be made; the arrow is used)
    void fill_cursor_hover(ui::CursorContext& c);  // ui1 t01: hovered object -> cursor context (battle)
    void update_radar();

    // ---- BM2a benchmark mode (app_benchmark.cpp, benchmark.hpp) ----
    // Loading: the save is loading / its load screen is up; Settling: gameplay frames until settle_frames settled ones in a
    // row (the sim holds); Measuring: the measured ticks; Done: finished or failed (run() leaves its loop).
    struct BenchState {
        enum class Phase : uint8_t { Loading, Settling, Measuring, Done } phase = Phase::Loading;
        FixedFrameClock clock{1};
        uint32_t settled_run = 0, unsettled_run = 0;
        uint64_t warmup_frames = 0, unsettled = 0, excluded = 0, rt_ticks = 0, start_tick = 0;
        uint64_t settled_seen = 0, forced = 0, taa_discarded = 0;  // BM2b: settled measured frames, forced redraws, TAA undos
        bool counted = false, ticked = false, tick_missed = false;  // the last frame: counted / ran a tick / a due tick did not run
        bool drawn = false;     // render_frame recorded and submitted a frame (not minimized / out of date)
        uint64_t retry_boxes = 0;  // retry_boxes_ at the start: a new one = the mission was lost
        // Story UI notifications (STORY_SPEECH_DONE, battle_end_closed, ...) wait here and go to the sim just before the
        // next tick, whichever frame raised them (the frame count between ticks varies).
        std::vector<sim::StoryUiNotification> story_pending;
        std::vector<float> frame_ms, render_ms, sim_ms;
        Clock::time_point t_load{}, t_start{}, t_progress{};  // load start, measuring start, last measured frame
        double wall = 0.0;
        double load_ms = 0.0;   // bm3: the load start to the first measured tick
        bool failed = false;
        std::string error;
        std::vector<std::string> autopilot;
    } bench_;
    bool bench_active() const { return opt_.bench.active(); }
    bool bench_fixed() const { return opt_.bench.fixed(); }
    bool bench_measuring() const { return bench_.phase == BenchState::Phase::Measuring; }
    bool bench_done() const { return bench_.phase == BenchState::Phase::Done; }
    // BM2b: fixed-frames measured frames are drawn TAA-provisional; bench_frame_end keeps or discards each one.
    bool bench_taa_provisional() const { return bench_fixed() && bench_measuring(); }
    void bench_apply_window();  // init, before the window: preset size, present mode, frame cap
    void bench_apply_video();   // init, after the video options: preset detail / AA / gamma / advanced options
    bool bench_start();         // init: the save loaded, no menus
    void bench_sample(const FrameTimes& closed);  // frame start: the previous frame's times
    void bench_step_sim();      // step_sim in benchmark mode
    void bench_autopilot();     // after the sim: answers what would wait for the player
    void bench_frame_end();     // after render: settle / count / finish
    void bench_fail(std::string why);
    void bench_trace();         // BM3 --bench-trace: the live objects per owner (count, centroid, extent) of the tick just run
    float bench_alpha() const;
    double bench_now() const;
    int bench_finish();         // report, CRC log, presentation check; the exit code

    // ---- bm2: scenario runs, the scripted camera, GPU timer scopes (app_bench_scenario.cpp, benchmark_scenario.hpp) ----
    struct BenchScenarioState {
        commands::CommandRecording orders;  // the scenario's recorded orders (replayed)
        bool have_orders = false;
        int view = 0;              // the world view the camera path drives (1 galactic, 2 battle; 0 = not chosen yet)
        tactical::Vec3 anchor{};   // camera_anchor start: that view's own camera target at its first gameplay frame
        rhi::GpuTimer gpu;
        bool gpu_on = false;
        uint32_t gpu_slot = UINT32_MAX, gpu_frame_scope = UINT32_MAX;  // this frame's slot / "frame" scope
        std::vector<uint8_t> slot_counted;  // per frame slot: the frame last recorded there counted
        std::vector<std::pair<std::string, std::vector<float>>> gpu_ms;  // per scope, first-seen order
        uint64_t gpu_frames = 0;
        bool not_comparable = false;
    } bscen_;
    bool bench_scenario_start();  // bench_start: the scenario's orders, the GPU timer
    const commands::CommandRecording* bench_replay() const { return bscen_.have_orders ? &bscen_.orders : nullptr; }
    // The scripted camera over the drawn view's own camera (view 1 galactic, 2 battle); false = not applied.
    bool bench_camera(ui::CameraParams& cp, int view, uint32_t w, uint32_t h);
    rhi::GpuTimer* bench_gpu_timer() { return bscen_.gpu_on ? &bscen_.gpu : nullptr; }
    void bench_gpu_begin(rhi::Frame& f, VkCommandBuffer cmd);  // render_frame, first: the slot's previous results, "frame" opens
    void bench_gpu_end(VkCommandBuffer cmd);                   // render_frame, before the submit: "frame" closes
    void bench_gpu_counted(bool counted);                      // bench_frame_end: whether this frame's slot counts
    void bench_scenario_end(BenchReport& r);  // bench_finish: GPU stats, fingerprint check, --bench-make output

    bool init();
    void shutdown();
    void frame();
    void handle_menu_actions(const std::vector<ui::MenuAction>& acts);
    // modc t08: the mod picker (app_mods.cpp). init_mod_picker lists the installed mods and sets MenuProviders::mods;
    // check_mod runs the load report of a row on a worker thread (one at a time; the newest request waits), poll_mod_check
    // hands a finished one to the dialog; launch_mod restarts the exe with the row's mod arguments and quits.
    void init_mod_picker(ui::MenuProviders& mp);
    void check_mod(const std::string& id);
    void poll_mod_check();
    void launch_mod(const std::string& id);
    std::string game_root_;                          // the FoC folder the data was loaded from
    std::vector<std::string> workshop_libraries_;    // Steam library folders (vfs::find_steam_libraries)
    std::vector<game::InstalledMod> installed_mods_;
    std::future<game::ModCheckReport> mod_check_;    // the running check (valid() while one runs)
    std::string mod_check_id_, mod_check_next_;      // its row id; the row asked for while it ran
    bool mod_check_queued_ = false;
    std::map<std::string, std::vector<std::string>> mod_launch_;  // checked rows: the arguments that start them
    bool start_session(const SessionSpec& spec);
    void session_started(const std::vector<std::string>& diag);  // the UI / presentation reset for a new session_
    void end_session();
    // N1 / t09: the Save / Load / Delete requests of the menus.
    void save_game_as(const std::string& typed);
    bool load_saved_game(const std::string& name);
    bool load_save_file(const std::string& path, const commands::CommandRecording* replay = nullptr);  // BM2a: load_saved_game by path (bm2: with a replay)
    void delete_saved_game(const std::string& name);
    void save_message(std::u32string caption, const std::string& detail);
    std::string session_title() const;  // the campaign's display name / the skirmish map (save descriptions)
    void begin_battle();
    void start_battle_view();
    void end_battle();
    // QA t11 / t16: the battle load screen, filled from the planet's text, is on screen before the blocking loads.
    std::u32string battle_place_name(PlanetIndex planet) const;
    void planet_type_info(ui::PlanetInfoData& d, objects::TypeIndex t) const;
    // UI5 t07: `mission` (a story-linked LINK_TACTICAL battle) opens IDD_LAND / SPACE_MISSION_LOAD_DIALOG (UI.md §5.1).
    bool open_battle_load(PlanetIndex planet, bool space, bool mission = false);
    void end_load_intro_frame();
    void post_tick();
    void step_sim();
    void galactic_input();
    void battle_input();
    // refixf t08: the world pick tests over the top tactical mode (null outside a battle). Valid until the next tick.
    const ui::WorldPickTests* world_pick_tests();
    void submit(commands::EventData ev, bool voice = true);
    void respond(const commands::EventData& ev);
    bool render_frame();
    void build_galactic_hud(ui::GalacticHudFrame& f, std::vector<ui::GalacticPlanetInfo>& planets);
    void build_tactical_hud();
    bool selection_can_repair() const;
    void tactical_hover(ui::TacticalHudFrame& hf, std::vector<ui::HudUnit>& units);
    bool reticle_orders(const ui::OrderContext& ctx, std::vector<commands::EventData>& cmds);
    bool present_settled() const;
    int32_t local_player() const { return session_ ? session_->local_player() : 0; }
    // cu9 t11: the session's galaxy / running tactical battle, nullptr without a session.
    const galactic::GalacticSim* galaxy() const { return session_ ? session_->galaxy() : nullptr; }
    const sim::TacticalBattle* battle() const { return session_ ? session_->battle() : nullptr; }
    // cu9 t08: the HUD icon of type `t` ("" when out of range), and the battle-end dialog's loss rows (owner -> type -> count;
    // `me`'s rows to your_losses, every other owner's to enemy_losses), shared by the tactical and auto-resolve ends.
    std::string type_icon(size_t t) const { return t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string(); }
    void add_loss_rows(ui::BattleEndInfo& info, const game::BattleLosses& lost, int32_t me) const;
    std::string local_faction() const;
    bool crc_check();
    void make_script();
    // Script helpers.
    ui::InputFrame idle_frame() const;
    std::vector<uint32_t> synced_selection() const;
};

// HudDrawList b's quads appended to a (every layer / sub-list, in order).
inline void merge_lists(ui::HudDrawList& a, const ui::HudDrawList& b) {
    for (uint32_t l = 0; l < ui::kHudLayers; ++l)
        for (uint32_t s = 0; s < ui::kHudSubLists; ++s)
            for (const ui::HudQuad& q : b.items(l, static_cast<ui::HudList>(s))) a.add(l, static_cast<ui::HudList>(s), q);
}
// Display name of planet row `p` (its type's Text_ID, else the row name).
inline std::u32string planet_label(const GameData& d, const ui::TextDatabase& text, const galactic::GalacticSim& g, PlanetIndex p) {
    if (p >= g.planets().size()) return {};
    const uint32_t t = g.planets()[p].type;
    if (t < d.types.size()) return type_label(d, text, static_cast<objects::TypeIndex>(t));
    return ui::utf8_to_utf32(p < g.map().planets.name.size() ? g.map().planets.name[p] : std::string());
}

// UTF-32 -> UTF-8 for the console log lines.
inline std::string utf8_of(std::u32string_view s) {
    std::string o;
    for (const char32_t c : s) {
        const uint32_t v = static_cast<uint32_t>(c);
        if (v < 0x80u) {
            o.push_back(static_cast<char>(v));
        } else if (v < 0x800u) {
            o.push_back(static_cast<char>(0xC0u | (v >> 6)));
            o.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
        } else if (v < 0x10000u) {
            o.push_back(static_cast<char>(0xE0u | (v >> 12)));
            o.push_back(static_cast<char>(0x80u | ((v >> 6) & 0x3Fu)));
            o.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
        } else {
            o.push_back(static_cast<char>(0xF0u | (v >> 18)));
            o.push_back(static_cast<char>(0x80u | ((v >> 12) & 0x3Fu)));
            o.push_back(static_cast<char>(0x80u | ((v >> 6) & 0x3Fu)));
            o.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
        }
    }
    return o;
}

}  // namespace alamo_game_app
