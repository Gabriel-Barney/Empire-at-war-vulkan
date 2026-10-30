// alamo_game (P host): the playable game exe. Per frame (MAIN_LOOP.md order): pump the window, capture input (WM_CHAR
// text included), route it (dialogs -> HUD -> world), menus / load screens / HUD / camera / click orders, at most the
// paced sim tick(s), snapshots of the finished ticks, render (world + HUD + dialogs), audio update.
//
//   main menu -> Single Player -> Custom Campaign  -> galactic map -> a human conflict -> load screen -> tactical battle
//                                                      on the planet's map -> back to the galactic map
//             -> Single Player -> Skirmish         -> load screen -> space battle on the chosen map -> main menu
//
// Usage: alamo_game [--game DIR] [--width W --height H] [--hidden] [--audio device|null|off] [--seed S] [--workers N]
//                   [--frames N] [--battle-max-frames N] [--cache-dir DIR] [--mailbox|--immediate] [MODPATH=..]
//        alamo_game --smoke skirmish|campaign --png-dir DIR [...]   scripted input through the real menus (tests);
//                   ends with the presentation-on/off check (the run's per-tick CRC log == a fresh presentation-free
//                   replay of its recorded command stream) and 0 validation messages. Exit 0 "skipped" without data.
// Game data: --game DIR or ALAMO_FOC_DATA (the FoC "corruption" folder).
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/game/attract_mode.hpp"
#include "alamo/game/battle_summary.hpp"
#include "alamo/game/game_audio_host.hpp"
#include "alamo/game/game_controls.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/game/hud_feeds.hpp"
#include "alamo/game/radar_feed.hpp"
#include "alamo/game/save_games.hpp"
#include "alamo/game/story_ui.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/render/galactic/galactic_session.hpp"
#include "alamo/render/png.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/corruption.hpp"  // Int-final: corruption panel
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

namespace {

using namespace alamo;
using namespace alamo::game;
using Clock = std::chrono::steady_clock;

std::string env_or_empty(const char* name) {
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

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
uint64_t final_crc(const CrcLog& log) { return log.records.empty() ? 0 : log.records.back().combined; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}

// The planet row of a galactic planet object id (kInvalidPlanet when none).
PlanetIndex planet_index(const galactic::GalacticSim& g, uint32_t planet_id) {
    for (size_t i = 0; i < g.planets().size(); ++i)
        if (g.store().exists(g.planets()[i].object) && static_cast<uint32_t>(g.store().id(g.planets()[i].object)) == planet_id)
            return static_cast<PlanetIndex>(i);
    return kInvalidPlanet;
}

// WM_CHAR text for GUI edit boxes: rhi::Window::take_text (printable characters only; Backspace / Enter / Tab / Esc reach
// the GUI as key presses).
std::u32string printable(std::u32string s) {
    std::u32string out;
    for (const char32_t c : s)
        if (c >= 0x20 && c != 0x7F) out.push_back(c);
    return out;
}
struct Options {
    std::string game, cache_dir, png_dir, smoke;
    std::vector<std::string> game_args;
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
};

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_game [--game DIR] [--width W] [--height H] [--hidden] [--audio device|null|off] [--seed S]\n"
                 "                  [--workers N] [--frames N] [--battle-max-frames N] [--cache-dir DIR] [--mailbox|--immediate]\n"
                 "                  [--frame-cap N|display] [--video-options FILE] [--saves-dir DIR]\n"
                 "                  [--movie NAME]... [--no-intro] [--attract] [--smoke skirmish|land|campaign --png-dir DIR] [MODPATH=..]\n"
                 "                  [FALLBACKPATH=..]\n"
                 "       game data: --game DIR or ALAMO_FOC_DATA (the corruption folder)\n");
    return 2;
}

enum class Screen : uint8_t { Menu, SessionLoad, Galactic, BattleLoad, Battle, Quit, Movie };
constexpr int32_t kLoadIntroMaxFrames = 30;  // QA t11: the most frames the sim waits for the load screen's textures
constexpr uint32_t kBattleEndHoldFrames = 150;  // [port] Playtest QA D1: 5 s between the victory and the summary (the last break-up plays)
constexpr uint64_t kLoadIntroMaxTicks = 30;  // sim ticks a Fight answer may take to push its battle
const char* screen_name(Screen s) {
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
    // [port: Gabriel] advanced video: anti-aliasing (TAA), ambient occlusion and anisotropic filtering. The detail sliders
    // and the soft shadow / heat / bloom checks reach the renderer in apply_detail_options (game_display.hpp, t10 / t11). world_view_: the last
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
    bool smoke_ok_ = true;
    std::vector<std::string> notes_;
    uint64_t battles_seen_ = 0;
    size_t space_map_count_ = 0;  // skirmish list rows before the first land map
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
    // Gameplay options (IDD gameplay dialog) [port: kept for the session, no profile file].
    ui::GameplaySettings gameplay_;
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
    std::vector<DisplayMode> display_modes_;  // the video options' resolution list (primary display, game_display.hpp)
    uint64_t video_applies_ = 0;
    bool menu_dialog_open() const;  // the in-game menu or one of its option / save / load / quit dialogs is open
    uint32_t reinforce_unit_ = 0;       // reinforcement chosen on the pad, waiting for its drop point (0 = none)
    // P1 move acknowledgement (P1_DOUBLE_CLICK_MOVE.md §2.3): GameConstants values by ui::AckEffect / ui::AckRadar index.
    std::array<std::string, 4> ack_fx_{};
    std::array<std::string, 3> ack_radar_{};
    float ack_scale_land_ = 1.0f, ack_scale_space_ = 5.0f;  // GUI_Move_Acknowledge_Scale_Land / _Space
    uint64_t move_acks_ = 0, move_ack_kills_ = 0, move_acks_drawn_ = 0;
    void move_ack(const ui::MoveAck& a);
    // P1_CAMERA_INPUT.md §3.1 options -> CameraTuning (live play only; smoke / replay runs keep `t`).
    bool live_input() const { return opt_.smoke.empty() && !opt_.hidden; }
    ui::CameraTuning option_tuning(ui::CameraTuning t) const;
    void apply_camera_tuning();
    uint64_t reinforce_drops_ = 0;
    // Information screens fed from the sim (game_info.hpp): holocron history, story cursor, campaign tallies.
    Holocron holo_;
    size_t story_cursor_ = 0;
    bool summary_completed_ = false;
    BattleTally tally_;
    bool campaign_over_ = false;
    galactic::VictoryRules victory_rules_;  // Int-verify: the campaign's victory conditions (start_session)
    bool choice_open_ = false;
    // GALACTIC_DIALOGS §1 battle prompt: the banner (pending_battle_shell / story_pending_battle_shell) fed through
    // GalacticHudFrame::battle while choice_open_; its clicks are HudAction BattleBegin / BattleAutoResolve.
    struct BattlePrompt {
        PlanetIndex planet = kInvalidPlanet;
        bool land = false;
        bool story_pending = false;   // §1.4 single-button shell (story lock): no Auto-Resolve
        std::u32string title;         // "<planet>: SPACE / PLANETARY BATTLE IMMINENT!" (log; the ui composes its own)
        std::u32string planet_name;
        BattleForceSummary left, right;  // graph_left = the local player's force, graph_right = the enemy's
    } prompt_;
    // §3.1: the local player's Auto-Resolve choice waiting for its conflict-log entry; the log cursor for result lines.
    bool auto_pending_ = false;
    PlanetIndex auto_planet_ = kInvalidPlanet;
    size_t ar_log_seen_ = 0;
    bool battle_end_auto_ = false;    // the open battle-end screen follows an auto-resolve
    // §2 mission holocron: the chapters of Story_Dialog events (TITLE + body lines), newest last; the b_story_arc_g flash.
    struct HolocronChapter {
        std::u32string title;
        std::vector<std::u32string> lines;
        std::vector<ui::Rgba8> colors;
        std::string dialog;
        int32_t chapter = -1;
    };
    std::vector<HolocronChapter> holocron_chapters_;
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
    void open_battle_prompt(const galactic::GalacticSim& g, const galactic::ConflictInfo& c);
    std::string representative_icon(std::span<const objects::ObjectHandle> units, bool hero_filter = false) const;
    void answer_battle_prompt(bool auto_resolve);
    int32_t summary_chapter_ = -1;  // the story summary's selected chapter (-1 = the newest)
    void auto_resolve_result(const galactic::GalacticSim& g, const galactic::ConflictLogEntry& e);
    void add_holocron_chapter(const ui::StoryDialogInfo& info, const StoryDialogMeta& meta);
    void screen_line(std::u32string text, ui::Rgba8 color);
    std::u32string txt(std::string_view id, std::u32string_view fallback) const;
    std::u32string faction_label(int32_t player) const;
    uint64_t choices_ = 0, auto_resolves_ = 0, story_dialogs_ = 0, ency_shown_ = 0, retry_boxes_ = 0, story_cues_ = 0;
    uint64_t withdraws_ = 0, tactical_auto_resolves_ = 0, auto_results_ = 0;
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
    // Planet type shown for battle planet row `p`: in a skirmish the map's planet (skirmish_map_planet_type, [port]),
    // else / when the map names none the row's own type.
    uint32_t display_planet_type(const galactic::GalacticSim& g, PlanetIndex p) const;
    void update_display_rate(bool force);
    void set_present_mode(rhi::PresentMode m);
    float render_alpha();
    void show_battle_end();
    int32_t galactic_faction_icon(const galactic::GalacticSim& g, int32_t player) const;
    ui::Rgba8 faction_rgba(const galactic::GalacticSim& g, int32_t player, bool text, ui::Rgba8 fallback) const;
    void planet_icons(const galactic::GalacticSim& g, PlanetIndex row, ui::GalacticPlanetInfo& out) const;
    ui::PlanetInfoData planet_info_data(const galactic::GalacticSim& g, PlanetIndex row) const;
    void show_auto_resolve_end(const galactic::ConflictLogEntry& e);  // ui ask 10: the battle-end screen after an auto-resolve
    void build_panels(std::vector<ui::HudDrawList>& lists);
    void update_cursor(ui::HudDrawList& dl);
    void update_radar();

    bool init();
    void shutdown();
    void frame();
    void handle_menu_actions(const std::vector<ui::MenuAction>& acts);
    bool start_session(const SessionSpec& spec);
    void session_started(const std::vector<std::string>& diag);  // the UI / presentation reset for a new session_
    void end_session();
    // N1 / t09: the Save / Load / Delete requests of the menus.
    void save_game_as(const std::string& typed);
    bool load_saved_game(const std::string& name);
    void delete_saved_game(const std::string& name);
    void save_message(std::u32string caption, const std::string& detail);
    std::string session_title() const;  // the campaign's display name / the skirmish map (save descriptions)
    void begin_battle();
    void start_battle_view();
    void end_battle();
    // QA t11 / t16: the battle load screen, filled from the planet's text, is on screen before the blocking loads.
    std::u32string battle_place_name(PlanetIndex planet) const;
    void planet_type_info(ui::PlanetInfoData& d, objects::TypeIndex t) const;
    bool open_battle_load(PlanetIndex planet, bool space);
    void end_load_intro_frame();
    void post_tick();
    void step_sim();
    void galactic_input();
    void battle_input();
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
    std::string local_faction() const;
    bool crc_check();
    void make_script();
    // Script helpers.
    ui::InputFrame idle_frame() const;
    std::vector<uint32_t> synced_selection() const;
};

// ------------------------------------------------------------------------------------------------ init
bool App::init() {
    const std::string root = opt_.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt_.game;
    if (root.empty()) {
        std::fprintf(stderr, "alamo_game needs --game <corruption dir> or ALAMO_FOC_DATA\n");
        return false;
    }
    const auto t0 = Clock::now();
    std::string err;
    if (!load_game_data(root, opt_.game_args, data_, &err)) {
        std::fprintf(stderr, "game data: %s\n", err.c_str());
        return false;
    }
    std::printf("data: %zu object types, %zu factions, %zu campaigns in %.0f ms\n", data_.types.size(), data_.rules.factions.size(),
                data_.db.campaigns().size(), ms_since(t0));
    js_ = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{opt_.workers});
    if (!win_.create({"Alamo - Empire at War", opt_.width, opt_.height, true, !opt_.hidden})) return false;
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::ContextDesc cd;
    cd.app_name = "alamo_game";
    cd.presentation = true;
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx_.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return false;
    }
    std::printf("device %s, validation %s%s\n", ctx_.caps().name.c_str(), ctx_.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    // [port] the saved video options: present mode, frame cap, hardware mouse, advanced video (the resolution below).
    video_profile_path_ = !opt_.video_options.empty() ? opt_.video_options : live_input() ? default_video_profile_path() : std::string();
    have_video_profile_ = load_video_profile(video_profile_path_, video_profile_);
    detail_options_ = live_input() || have_video_profile_;
    if (have_video_profile_) {
        if (!opt_.mode_set)
            opt_.mode = !video_profile_.vsync ? rhi::PresentMode::Immediate
                        : video_profile_.vsync_mode == ui::VsyncMode::Mailbox ? rhi::PresentMode::Mailbox : rhi::PresentMode::Fifo;
        if (!opt_.frame_cap_set) opt_.frame_cap = video_profile_.frame_cap;
        hardware_mouse_ = video_profile_.hardware_mouse;
        sync_anti_alias(video_profile_);
        sync_detail(video_profile_);  // t43
        adv_video_ = video_profile_.advanced;
        std::printf("video options: %s\n", video_profile_path_.c_str());
    }
    rhi::SwapchainDesc sd;
    sd.hinstance = win_.native_instance();
    sd.hwnd = win_.native_handle();
    sd.width = win_.width();
    sd.height = win_.height();
    sd.mode = opt_.mode;
    if (!sc_.init(ctx_, sd)) return false;
    vsync_mode_ = opt_.mode == rhi::PresentMode::Mailbox ? ui::VsyncMode::Mailbox : ui::VsyncMode::Fifo;
    if (have_video_profile_ && !opt_.mode_set) vsync_mode_ = video_profile_.vsync_mode;  // kept while v-sync is off
    update_display_rate(true);
    std::printf("present modes: mailbox %s, immediate %s\n", sc_.supports(rhi::PresentMode::Mailbox) ? "yes" : "no",
                sc_.supports(rhi::PresentMode::Immediate) ? "yes" : "no");
    if (!gpu_.init(ctx_, data_, *js_, opt_.cache_dir, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (!targets_.create(gpu_.alloc, gpu_.ring.frames_in_flight(), win_.width(), win_.height())) return false;
    gpu_.renderer.set_anisotropy(static_cast<uint32_t>(adv_video_.anisotropy));
    // Menus: skirmish maps from the data, audio volumes from the host.
    ui::MenuProviders mp;
    // Skirmish maps: space maps (Kuat first), then land maps (a land map starts a land skirmish).
    std::vector<std::string> maps = skirmish_space_maps(data_);
    space_map_count_ = maps.size();
    for (const std::string& m : skirmish_land_maps(data_)) maps.push_back(m);
    mp.camera_settings = [this] { return camera_; };
    mp.gameplay = [this] { return gameplay_; };
    // Video options: the primary display's modes (the current window size selected, else the largest mode under it); the
    // window is always windowed [port: no exclusive full screen]; v-sync = a FIFO or Mailbox present mode (the port's
    // V-sync mode combo picks which; off = Immediate), plus the port's frame rate cap and display line.
    display_modes_ = display_modes(win_.native_handle());
    if (have_video_profile_ && live_input() && video_profile_.mode.width && video_profile_.mode.height) {  // the saved mode
        std::string line;
        apply_display_mode(win_.native_handle(), video_profile_.mode, &line);
        std::printf("video: %s\n", line.c_str());
        update_display_rate(true);
    }
    mp.video = [this] {
        ui::VideoSettings v;
        for (const DisplayMode& m : display_modes_) v.resolutions.push_back(ui::utf8_to_utf32(display_mode_label(m)));
        v.resolution = display_mode_index(display_modes_, win_.width(), win_.height(), refresh_hz_);
        v.detail = video_profile_.detail;  // the original's sliders: saved with the profile, applied each frame (t11)
        v.anti_alias = video_profile_.anti_alias;
        v.gamma = video_profile_.gamma;
        v.windowed = true;
        v.vsync = opt_.mode != rhi::PresentMode::Immediate;
        v.vsync_mode = vsync_mode_;
        v.mailbox_available = sc_.supports(rhi::PresentMode::Mailbox);
        v.frame_cap = opt_.frame_cap;
        const VkPresentModeKHR pm = sc_.present_mode();
        const char* used = pm == VK_PRESENT_MODE_MAILBOX_KHR ? "v-sync (Mailbox)" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "no v-sync" : "v-sync (FIFO)";
        v.display_info = ui::utf8_to_utf32((refresh_hz_ ? "Display " + std::to_string(refresh_hz_) + " Hz, " : std::string("Display ")) +
                                           "presenting with " + used +
                                           (limiter_.fps() ? ", capped at " + std::to_string(limiter_.fps()) + " fps" : std::string()));
        v.device = ui::utf8_to_utf32(ctx_.caps().name);
        v.hardware_mouse = hardware_mouse_;
        return v;
    };
    mp.advanced_video = [this] { return adv_video_; };
    // N1 / t09: the Save / Load dialogs' list (newest first), the Save dialog's blocker and the overwrite question.
    saves_dir_ = !opt_.saves_dir.empty()                          ? opt_.saves_dir
                 : !opt_.smoke.empty() && !opt_.png_dir.empty() ? opt_.png_dir + "/saves"
                                                                : default_saves_dir();
    std::printf("saves: %s\n", saves_dir_.empty() ? "(no folder: %LOCALAPPDATA% unset)" : saves_dir_.c_str());
    mp.saves = [this] {
        std::vector<ui::SaveGameInfo> v;
        for (const SaveEntry& e : list_saves(saves_dir_)) {
            ui::SaveGameInfo i;
            i.name = e.name;
            i.label = ui::utf8_to_utf32(e.readable ? save_label(e.meta) : e.name + "  -  (unreadable: " + e.error + ")");
            v.push_back(std::move(i));
        }
        return v;
    };
    mp.save_blocker = [this] {
        if (saves_dir_.empty()) return std::u32string(U"No saved games folder (%LOCALAPPDATA% is not set)");
        if (!session_ || demo_session()) return std::u32string(U"No game is running");
        return ui::utf8_to_utf32(session_->check_save());
    };
    mp.save_exists = [this](const std::string& name) { return game::save_exists(saves_dir_, name); };
    mp.skirmish_maps = [maps] {
        std::vector<ui::SkirmishMapInfo> v;
        for (const std::string& m : maps) {
            ui::SkirmishMapInfo i;
            i.map = m;
            v.push_back(i);
        }
        return v;
    };
    if (!ui_.init(gpu_, data_, std::move(mp), &err)) {
        std::fprintf(stderr, "UI: %s\n", err.c_str());
        return false;
    }
    ui_.resize(win_.width(), win_.height());
    // OI host: the hotkey table's defaults (W A S D movement, arrows / numpad yaw-pitch-roll, Pause) and the queue
    // modifier drive the camera / order bindings from the first frame (before: ui::Bindings' own defaults until the
    // controls dialog was accepted).
    sync_control_bindings(ui_.hotkeys, camera_, ui_.bindings);
    attract_.set_config(load_attract_config(data_.db.constants()));
    {  // P1 move acknowledgement constants (P1_DOUBLE_CLICK_MOVE.md §2.3 / §5)
        const data::GameConstants& gc = data_.db.constants();
        for (size_t i = 0; i < ack_fx_.size(); ++i)
            if (const auto v = gc.get(ui::ack_effect_constant(static_cast<ui::AckEffect>(i)))) ack_fx_[i] = std::string(*v);
        for (size_t i = 0; i < ack_radar_.size(); ++i)
            if (const auto v = gc.get(ui::ack_radar_constant(static_cast<ui::AckRadar>(i)))) ack_radar_[i] = std::string(*v);
        ack_scale_land_ = gc.get_float("GUI_Move_Acknowledge_Scale_Land", ack_scale_land_);
        ack_scale_space_ = gc.get_float("GUI_Move_Acknowledge_Scale_Space", ack_scale_space_);
    }
    // Live play: the user's Windows double-click time / rectangle (P1_DOUBLE_CLICK_MOVE.md §0); smoke runs keep the defaults.
    if (live_input()) input_ = ui::InputState(ui::system_input_config());
    if (!audio_.init(data_, opt_.audio, opt_.seed ^ 0xA0D10u, &err)) std::fprintf(stderr, "audio: %s (continuing without)\n", err.c_str());
    ui_.gui.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    ui_.galactic_hud.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    ui_.tactical_hud.set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
    faction_colors_ = render::load_faction_colors(*data_.files);
    gview_.init(gpu_, data_, faction_colors_, render::load_route_line_style(*data_.files));
    apply_camera_tuning();
    // P4 screens / panels / cursor / radar / hotkeys.
    screens_.init(ui_.gui);
    info_.init(ui_.gui);
    orders_.set_keyboard_orders(false);  // keyboard orders come from HotkeyDispatcher (the 153-slot table)
    {
        std::vector<uint8_t> xml;
        if (data_.files->read("Data\\XML\\MousePointers.xml", xml)) {
            const data::XmlDocument doc = data::XmlDocument::parse(xml, "MousePointers.xml");
            if (doc.ok() && cursor_table_.parse(doc.root()) > 0) {
                cursor_table_.resolve_frames([this](std::string_view n) {
                    if (ui_.atlas.find(n)) return true;
                    // ui ask 1: a silent existence check (the texture cache logged every missing *_01.TGA frame).
                    for (const std::string& c : assets::texture_candidates(n))
                        if (data_.files->exists(c)) return true;
                    return false;
                });
                cursor_.init(cursor_table_, &ui_.atlas, [this](std::string_view t) { return ui_.pages.resolve(t); });
                cursor_ok_ = true;
            }
        }
        std::string e;
        radar_settings_.load(*data_.files, &e);
        // HUD_GAPS §1.5: the hard-point reticle textures (GameConstants) and their stand-alone DDS pages.
        if (data_.files->read("Data\\XML\\GameConstants.xml", xml)) {
            const data::XmlDocument gc = data::XmlDocument::parse(xml, "GameConstants.xml");
            if (gc.ok()) {
                reticles_.parse(gc.root());
                ency_cfg_ = ui::tooltip_config_from(gc.root());  // HUD_GAPS §2.2 delays / fade / min display
            }
        }
        hud_images_ = [this](std::string_view t) { return ui_.pages.resolve(t); };
        if (const char* dbg = std::getenv("ALAMO_HUD_HIT_DEBUG")) {
            hud_hit_debug_ = dbg[0] != '\0' && dbg[0] != '0';
            int x = -1, y = -1;
            if (std::strchr(dbg, ',') && std::sscanf(dbg, "%d,%d", &x, &y) == 2) {
                hud_hit_px_ = x;
                hud_hit_py_ = y;
            }
        }
        ency_.init(ui_.components, ui_.atlas, *ui_.fonts, &ui_.text);
        ency_cfg_.delay_tooltips = gameplay_.delay_tooltips;
        ency_.set_config(ency_cfg_);
        std::vector<std::string> names;
        for (ui::HudPanel* p : panels()) {
            p->init(ui_.components, ui_.atlas, *ui_.fonts, &ui_.text);
            p->set_sfx_hook([this](std::string_view ev, int32_t) { audio_.ui(ev); });
            for (const std::string& n : p->shell_names()) names.push_back(n);
        }
        std::vector<std::string> missing;
        panel_shells_.load(ui_.components, [this](std::string_view n) { return gpu_.cache->model(n); }, names, &missing);
        const render::TextureSource tex = [this](std::string_view n, assets::TextureRole role) { return gpu_.cache->texture(n, role); };
        panel_shells_ok_ = !panel_shells_.empty() && panel_drawer_.upload(gpu_.renderer, panel_shells_, tex, &e);
        panel_drawer_.wait_uploads(gpu_.up);
        for (const ui::ShellSet::Shell& s : panel_shells_.shells()) panel_shells_.set_visible(s.component, false);
        for (ui::HudPanel* p : panels()) {
            p->resize(win_.width(), win_.height());
            panel_shells_.apply(p->layout());
        }
    }
    if (!opt_.smoke.empty()) make_script();
    if (!opt_.movies.empty() && next_movie()) screen_ = Screen::Movie;
    else show_menu_after_intro();
    return true;
}

// The main menu after the intro movies (or at once without them).
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
    if (!script_.empty() || !attract_.armed()) return false;
    const bool input = raw.keys_pressed != ui::KeySet{} || raw.buttons_pressed != 0 || raw.wheel != 0.0f || moved;
    using D = AttractController::Dialogs;
    const D dialogs = ui_.gui.find("IDD_MAIN_MENU_DIALOG") ? D::MainMenu : ui_.gui.top() ? D::Other : D::None;
    if (attract_.update(now_s_, input, dialogs) == AttractController::Request::None) return false;
    if (start_attract_map()) return true;
    stop_attract();
    return false;
}

// The demo session (0x22f10): a space skirmish with no human - player 0 Rebel (BasicRebel), the other side Underworld
// (AI_Player_Underworld) - on the controller's map. EaW .rec entries (Gabriel 2026-10-28): the recording is not replayed;
// its header names the map ([port] scan, attract_rec_map) and the same live demo runs there; no map path -> the entry is
// skipped with a warning. [port] fallback: the first space skirmish map when the entry does not load as a space map.
bool App::start_attract_map() {
    SessionSpec s;
    s.kind = SessionKind::Skirmish;
    s.attract = true;
    s.seed = opt_.seed + static_cast<uint32_t>(attract_.starts());
    s.battle_max_frames = static_cast<uint32_t>(std::max(attract_.cycle_delay(), 60.0) * 30.0) + 300u;  // the cycle reloads first
    s.skirmish.human_faction = "Rebel";
    s.skirmish.ai_faction = "Underworld";
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
    const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
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
    // Movies.xml Cannot_Skip: the skip keys do nothing for that movie.
    const bool skip = !full_movie_.cannot_skip() &&
                      (input_.key_pressed(ui::vk::kEscape) || input_.key_pressed(ui::vk::kSpace) || input_.button(ui::Button::Left).click);
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
            std::string up = name;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::string err;
            MovieSlot& slot = up.find("_LOOP") != std::string::npos ? advisor_movie_ : full_movie_;
            if (!slot.open(data_, audio_, name, &slot == &advisor_movie_, &err)) std::fprintf(stderr, "Lua movie %s: %s\n", name.c_str(), err.c_str());
        } else if (e.op == lua::PresentationOp::StopBinkMovie) {
            stop_movies();
        } else if (e.op == lua::PresentationOp::ActivateRetryDialog) {  // mission lost: Retry / Quit message box
            ui::MessageBoxSpec m;
            m.tag = "retry";
            m.caption = U"Mission failed";
            m.lines = {U"Retry the mission from the start, or quit to the main menu."};
            m.buttons = {U"Retry", U"Quit"};
            if (screens_.show_message(m)) ++retry_boxes_;
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
    for (const sim::StoryUiNotification& n : story_ui_.take())
        if (session_->notify_story_ui(n)) ++story_ui_sent_;
}

void App::shutdown() {
    set_os_cursor_hidden(false);
    restore_display_mode();  // [port] the monitor mode a video option switched
    stop_movies();
    gpu_.ring.wait_idle();
    ctx_.wait_idle();
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
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
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
    return true;
}

// The presentation side of a session that just started or was loaded (session_ is set).
void App::session_started(const std::vector<std::string>& diag) {
    const SessionSpec& spec = session_->spec();
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
    summary_chapter_ = -1;
    screen_lines_.clear();
    victory_rules_ = {};  // Int-verify: galactic victory conditions of the campaign (none for a skirmish)
    if (const galactic::GalacticSim* vg = session_->galaxy(); vg && spec.kind == SessionKind::Campaign)
        victory_rules_ = galactic::load_victory_rules(data_.db, vg->map().campaign_name, vg->map());
    orders_ = ui::OrderTranslator{};
    orders_.set_keyboard_orders(false);  // keyboard orders come from HotkeyDispatcher
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
    ui_.set_faction(local_faction());
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
    targeting_.cancel();  // nor a targeted ability
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
    // QA t11: the dialog first, the blocking BattleView::start only after it has been presented (a Fight answer or a
    // skirmish start opened it even earlier, before the tactical push's own load).
    if (ui_.load.is_open()) load_intro_frames_ = 1;
    else if (!open_battle_load(planet, space)) notes_.push_back("load screen dialog missing");
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
    bview_.stop(gpu_);
    if (ui_.load.is_open()) ui_.load.close();
    orders_.set_selection({});
    queues_.clear();
    if (demo_session()) return;  // OI host: no battle-end screen; the attract controller moves on (screen_ stays Menu)
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
uint32_t App::panel_kind() const {
    if (screen_ != Screen::Galactic || !session_) return 0;
    if (const SpecialDialog* d = session_->special_dialog()) return d->kind;
    return panel_preview_;
}

// The local player's units at planet `pi` whose type has a Ground_Activated special ability, in g_special_ability row
// order [port]: orbit fleets in orbit order, members in order, then the surface.
std::vector<objects::ObjectHandle> App::special_units(const galactic::GalacticSim& g, PlanetIndex pi) const {
    std::vector<objects::ObjectHandle> out;
    if (pi == kInvalidPlanet) return out;
    const objects::ObjectStore& st = g.store();
    const galactic::SpecialRules& sp = g.rules().special;
    auto usable = [&](objects::ObjectHandle h) {
        if (!st.alive(h) || st.owner(h) != local_player()) return false;
        for (const uint32_t a : sp.type(static_cast<uint32_t>(st.type(h))).abilities)
            if (sp.abilities[a].style == galactic::ActivationStyle::GroundActivated) return true;
        return false;
    };
    for (const uint32_t fi : g.fleets_at(pi, local_player()))
        for (const objects::ObjectHandle h : g.fleets()[fi].contents)
            if (usable(h)) out.push_back(h);
    for (const objects::ObjectHandle h : g.planets()[pi].surface)
        if (usable(h)) out.push_back(h);
    return out;
}

// SpecialAbility kind 1 (activate) at the selected planet by the g_special_ability row's hero (GalacticHud rows from
// special_units); hero_id 0 = the first of them. False when none / refused.
bool App::activate_special_here(uint32_t hero_id) {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return false;
    const objects::ObjectStore& st = g->store();
    for (const objects::ObjectHandle h : special_units(*g, planet_index(*g, selected_planet_)))
        if ((hero_id == 0 || static_cast<uint32_t>(st.id(h)) == hero_id) &&
            session_->activate_special(static_cast<uint32_t>(st.id(h)), selected_planet_)) {
            ++activations_;
            return true;
        }
    return false;
}

// SFX / messages of the special events (local player only, P4_SIM_SYSTEMS §1.4, §2.3, §2.4, §3.3, §6.2). The "local error
// SFX" (0x140b301c8) is not identified: [port] the GUI negative feedback.
void App::special_feedback() {
    if (!session_) return;
    using K = galactic::GalacticEventKind;
    auto type_name = [this](uint32_t t) { return t < data_.rules.types.size() ? data_.rules.types[t].name.c_str() : "?"; };
    bool unlock_sfx = false;
    for (const galactic::GalacticEvent& e : session_->take_special_events()) {
        const bool local = e.player == local_player();
        switch (e.kind) {
            case K::SpecialError:
                if (local) audio_.negative_feedback();
                break;
            case K::BlackMarketPurchase:
                if (local && e.other_player == 1) {
                    audio_.faction_sfx("SFXEvent_New_Construction_Options_Available");
                    audio_.faction_sfx("SFXEvent_Slice_Success");
                    ++special_sfx_;
                }
                break;
            case K::GalacticSabotage:
                if (local) {
                    audio_.faction_sfx("SFXEvent_Sabotage_Success");
                    ++special_sfx_;
                }
                break;
            case K::SuperWeaponHacked:
                if (local) {
                    audio_.faction_sfx("SFXEvent_Hack_Success");
                    ++special_sfx_;
                }
                break;
            case K::HeroRespawned:
                if (local && e.type < data_.types.size()) {
                    audio_.ui(data_.types.get_string(static_cast<objects::TypeIndex>(e.type), "SFXEvent_Hero_Respawned"));
                    ++special_sfx_;
                }
                break;
            case K::SabotageVigilance:  // TEXT_SABOTAGE_FAILURE_VIGILANCE to the saboteur's owner, _PREVENTED_ to the planet's
                if (local || e.other_player == local_player()) {
                    const char* id = local ? "TEXT_SABOTAGE_FAILURE_VIGILANCE" : "TEXT_SABOTAGE_PREVENTED_VIGILANCE";
                    std::printf("message: %s%s\n", id, ui_.text.find(id) ? "" : " (no text)");  // [port] no message log UI
                    ++special_msgs_;
                }
                break;
            case K::TypeUnlocked:  // REMAINING_GAPS 1.4 / 1.5: the local player's new construction options (one SFX per batch)
                if (local) {
                    std::printf("unlocked: %s (%s)\n", type_name(e.type),
                                e.value == 1 ? "tech sweep" : e.value == 2 ? "slice" : e.value == 4 ? "corruption" : "story / Lua");
                    if (!unlock_sfx) {
                        audio_.faction_sfx("SFXEvent_New_Construction_Options_Available");
                        ++special_sfx_;
                        unlock_sfx = true;
                    }
                }
                break;
            case K::Slice:  // REMAINING_GAPS 1.3 step 7: outcome messages (slicer owner; vigilance also to the planet owner)
                if (local || (e.value == 4 && e.other_player == local_player())) {
                    const char* id = e.value == 0   ? "TEXT_SLICE_SUCCESS"
                                     : e.value == 1 ? "TEXT_SLICE_FAILURE_CAPTURED"
                                     : e.value == 3 ? "TEXT_SLICE_FAILURE_CHEATED"
                                     : e.value == 4 ? (local ? "TEXT_SLICE_FAILURE_VIGILANCE" : "TEXT_SLICE_PREVENTED_VIGILANCE")
                                                    : nullptr;  // 2 lost effort: no message [port: none identified]
                    std::printf("slice: %s %s%s%s\n", type_name(e.type), e.value == 0 ? "unlocked" : e.value == 2 ? "lost effort" : "failed",
                                id ? ", message " : "", id ? id : "");
                    if (id) ++special_msgs_;
                    if (local && e.value == 0) {
                        audio_.faction_sfx("SFXEvent_Slice_Success");
                        ++special_sfx_;
                    }
                }
                break;
            default: break;
        }
    }
}
// --- end P4 host ---

void App::post_tick() {
    audio_.sim_tick();
    if (session_->take_reveal_sfx()) {  // GRC 1.3: a planet was revealed by the galactic pre-step (frame > 0)
        audio_.ui("SFXEvent_Planet_Revealed");
        ++reveal_sfx_;
    }
    special_feedback();  // P4 host
    if (session_->in_battle()) {
        if (!bview_.active()) {
            begin_battle();
            return;
        }
        bview_.capture(*session_);
        if (const sim::TacticalBattle* b = session_->battle()) {
            const objects::ObjectStore& st = session_->host().modes().top_tactical()->primary();
            audio_.battle_tick(*b, st, data_.types, local_player());
            {  // radar pings (deaths, capture-point transitions) and the story's blips: read-only presentation
                const lua::StoryModel* tac_story = nullptr;
#ifdef ALAMO_SIM_HAVE_STORY
                if (const sim::StorySession* sst = session_->host().story()) tac_story = sst->tactical_model();
#endif
                radar_feed_.battle_tick(*b, st, data_.types, ui_.hud_types, local_player(), tac_story, radar_settings_, radar_events_);
            }
        }
        // Selection mirror <- the synced selection once the local orders were drained.
        if (session_->host().frame() > last_submit_frame_) orders_.set_selection(synced_selection());
        std::vector<std::pair<uint64_t, Vec3>> pos;
        for (const render::SnapshotObject& o : bview_.frame().objects) pos.emplace_back(static_cast<uint64_t>(o.id), o.position);
        audio_.set_object_positions(std::move(pos));
    } else {
        if (bview_.active()) end_battle();
        if (session_) gview_.capture(session_->host());
    }
}

void App::step_sim() {
    if (!session_ || hold_sim_ || script_hold_ || paused_ || battle_end_open_ || story_summary_open_) return;
    if (load_intro_frames_ > 0) return;  // QA t11: the load screen reaches the screen before the push's blocking load
    const bool demo = screen_ == Screen::Menu && demo_session();  // OI host: the attract demo behind the main menu
    if ((screen_ == Screen::Menu && !demo) || screen_ == Screen::BattleLoad || screen_ == Screen::Quit) return;
    const bool smoke = !opt_.smoke.empty();
    if (demo && attract_.idle_paused()) return;  // the attract idle pause (rendering continues)
    if (demo && !bview_.active()) {  // its hidden skirmish galaxy runs fast until the battle is pushed
        for (int i = 0; i < 200 && demo_session() && !bview_.active(); ++i)
            if (session_->tick()) post_tick();
        return;
    }
    if (screen_ == Screen::SessionLoad) {  // skirmish: run the hidden galaxy fast until the battle is pushed
        for (int i = 0; i < 200 && session_ && screen_ == Screen::SessionLoad; ++i)
            if (session_->tick()) post_tick();
        return;
    }
    if (smoke) {
        const int n = screen_ == Screen::Galactic ? 16 : 4;
        const Screen at = screen_;  // a battle start / end ends the frame's ticks (the presentation catches up)
        for (int i = 0; i < n && session_ && screen_ == at; ++i)
            if (session_->tick()) post_tick();
        return;
    }
    const auto now = static_cast<uint32_t>(ms_since(t0_));
    if (session_->loop_iteration(now)) {
        prev_tick_ms_ = last_tick_ms_;
        last_tick_ms_ = now;
        post_tick();
    }
}

std::vector<uint32_t> App::synced_selection() const {
    std::vector<uint32_t> v;
    if (!session_) return v;
    for (const objects::ObjectId id : session_->host().selection(static_cast<uint8_t>(local_player()))) v.push_back(static_cast<uint32_t>(id));
    std::sort(v.begin(), v.end());
    return v;
}

// Unit responses (AUDIO.md 3.3): the representative unit = the lowest id of the order's units [port].
void App::respond(const commands::EventData& ev) {
    if (!session_) return;
    using T = commands::EventType;
    UnitResponse kind;
    std::vector<uint32_t> ids;
    switch (ev.type()) {
        case T::Select: kind = UnitResponse::Select; ids = ev.ids("ids"); break;
        case T::MoveToPosition: case T::MoveToObject: case T::MoveToRay: case T::MoveToPositionFacing: case T::MoveToRayFacing:
        case T::Facing:  // [port] a facing drag answers like a move
            kind = UnitResponse::Move;
            ids = (ev.type() == T::MoveToPositionFacing || ev.type() == T::Facing) && !ev.ids("units").empty() ? ev.ids("units")
                                                                                                                : orders_.selection();
            break;
        case T::Attack: kind = UnitResponse::Attack; ids = ev.ids("attackers").empty() ? orders_.selection() : ev.ids("attackers"); break;
        case T::Escort: kind = UnitResponse::Guard; ids = orders_.selection(); break;
        case T::StopMovement: kind = UnitResponse::Stop; ids = orders_.selection(); break;
        case T::MoveObjectToObject: kind = UnitResponse::FleetMove; ids = {ev.u("object")}; break;
        default: return;
    }
    if (ids.empty()) return;
    const uint32_t id = *std::min_element(ids.begin(), ids.end());
    const sim::GameMode* top = session_->host().modes().top();
    if (!top) return;
    const objects::ObjectStore& st = top->primary();
    objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(id));
    if (!st.alive(h)) return;
    if (kind == UnitResponse::FleetMove || (!top->tactical() && kind == UnitResponse::Select)) {
        // A galactic fleet answers with its first member.
        if (const galactic::GalacticSim* g = session_->galaxy())
            for (const galactic::FleetState& f : g->fleets())
                if (f.alive && f.object == h && !f.contents.empty() && st.alive(f.contents.front())) {
                    h = f.contents.front();
                    break;
                }
    }
    audio_.unit_response(data_.types, st.type(h), static_cast<uint64_t>(st.id(h)), kind, false, top->tactical() ? st.owner(h) : -1);
}

void App::submit(commands::EventData ev, bool voice) {
    if (!session_) return;
    if (voice) respond(ev);  // UiOrder::voice false: the double-click order replays no unit response (spec §1.4)
    const std::string name(ev.def().name);
    std::string detail;  // galactic moves: which fleet / planet / player (the reason alone hid the playtest failure)
    if (ev.type() == commands::EventType::MoveObjectToObject)
        detail = " (object " + std::to_string(ev.u("object")) + " -> target " + std::to_string(ev.u("target")) + ", player " +
                 std::to_string(local_player()) + ")";
    const commands::SubmitResult r = session_->submit(std::move(ev));
    if (!r.ok()) {
        std::fprintf(stderr, "submit %s refused: %s%s\n", name.c_str(), std::string(commands::reject_reason_name(r.status)).c_str(), detail.c_str());
        ++refused_;
    }
    if (std::getenv("ALAMO_GAME_TRACE")) std::printf("trace: submit %s at tick %" PRIu64 "\n", name.c_str(), session_->host().frame());
    last_submit_frame_ = session_->host().frame() + 1;
}

// Commands of this frame (translator, HUD, hotkeys, panels, screens) -> the session. Pause entries toggle the pause
// (a command-stream entry, GameSession::set_paused); a HUD Retreat asks for confirmation first (IDD_RETREAT_DIALOG).
void App::route_commands(std::vector<commands::EventData>& cmds, std::span<const uint8_t> voice) {
    for (size_t ci = 0; ci < cmds.size(); ++ci) {
        commands::EventData& e = cmds[ci];
        bool p = false;
        if (ui::read_pause_event(e, p)) {
            if (session_ && p != session_->pause_requested()) toggle_pause();
            continue;
        }
        // GRC 1.1: no fleet orders to a planet whose raw revealed byte is clear (the human hop gate would refuse them):
        // click, drag-to-move and the HUD fleet drag all end here.
        if (screen_ == Screen::Galactic && session_ &&
            (e.type() == commands::EventType::MoveToObject || e.type() == commands::EventType::MoveObjectToObject) &&
            !session_->move_target_revealed(e.u("target"))) {
            ++hidden_moves_dropped_;
            continue;
        }
        if (e.type() == commands::EventType::Retreat && screen_ == Screen::Battle && !ui_.gui.find(ui::dlg::kRetreat)) {
            screens_.show_retreat_confirm();
            continue;
        }
        submit(std::move(e), ci >= voice.size() || voice[ci] != 0);
    }
    cmds.clear();
    // OI host: the Pause hotkey flips its own idea of the state; keep it on the session's (a refused or ignored toggle).
    if (session_) hotkeys_.set_paused(session_->pause_requested());
}

void App::toggle_pause() {
    if (!session_) return;
    const bool want = !session_->pause_requested();
    if (session_->set_paused(want)) {
        ++pause_toggles_;
        hotkeys_.set_paused(want);
        std::printf("pause: %s at tick %" PRIu64 "\n", want ? "requested" : "resumed", session_->host().frame());
    }
}

void App::handle_hud_actions(const std::vector<ui::HudAction>& acts) {
    for (const ui::HudAction& a : acts) {
        switch (a.kind) {
            case ui::HudAction::Kind::GalacticSelect: {
                // A planet fleet button selects its fleet; the land-forces button names the planet: select the planet
                // (no fleet selection).
                bool planet = false;
                if (const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr; g && a.ids.size() == 1)
                    for (const galactic::PlanetState& ps : g->planets())
                        if (g->store().alive(ps.object) && static_cast<uint32_t>(g->store().id(ps.object)) == a.ids.front()) planet = true;
                if (planet) {
                    selected_planet_ = a.ids.front();
                    orders_.set_selection({});
                } else {
                    orders_.set_selection(a.ids);
                }
                break;
            }
            case ui::HudAction::Kind::BattleBegin: answer_battle_prompt(false); break;  // GALACTIC_DIALOGS §1.5
            case ui::HudAction::Kind::BattleAutoResolve: answer_battle_prompt(true); break;
            case ui::HudAction::Kind::ResumeGame:  // the Game Paused banner's Resume Game (pause_event(false))
                if (session_ && session_->pause_requested()) toggle_pause();
                break;
            case ui::HudAction::Kind::RadarCameraJump: bview_.camera().set_look_at({a.x, a.y, 0.0f}); break;
            case ui::HudAction::Kind::PlanetInfo: {  // GALACTIC_DIALOGS §9: the organiser's planet_info button -> IDD_PLANET_INFO
                // Single instance: a click while it is open does nothing (the first planet is kept). No pause, no §6.3 gates.
                if (screens_.planet_info_open()) break;
                const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
                const PlanetIndex row = g ? planet_index(*g, a.unit) : kInvalidPlanet;
                if (row == kInvalidPlanet) break;
                ui::PlanetInfoData d = planet_info_data(*g, row);
                d.planet = a.unit;
                if (!screens_.show_planet_info(d)) notes_.push_back("planet info dialog missing");
                break;
            }
            case ui::HudAction::Kind::BeginReinforcement:  // the drop point is the next left click in the world
                reinforce_unit_ = a.unit;
                targeting_.cancel();  // one modal pick at a time
                break;
            case ui::HudAction::Kind::BeginAbilityTargeting:  // the target is the next world click (TargetingController)
                if (screen_ == Screen::Battle && targeting_.begin(a)) reinforce_unit_ = 0;
                break;
            case ui::HudAction::Kind::TacticalBuy:  // Build_NN: Production {type_index, location = builder}
                if (screen_ == Screen::Battle && a.unit != 0 && a.value >= 0)
                    submit(commands::ev::production(static_cast<uint16_t>(a.value), a.unit));
                break;
            case ui::HudAction::Kind::TacticalCancel:  // tqueueNN: Production {type_index, location, cancel, queue, index}
                if (screen_ == Screen::Battle && a.value >= 0 && a.ids.size() >= 2)
                    submit(commands::ev::production(static_cast<uint16_t>(a.value), a.unit, true,
                                                    static_cast<uint8_t>(a.ids[0]), static_cast<uint8_t>(a.ids[1])));
                break;
            case ui::HudAction::Kind::HeroSelect:  // FIXPASS_GALACTIC §2.3 tactical single click: replace the selection
                if (a.unit && screen_ == Screen::Battle) submit(commands::ev::select({a.unit}));
                break;
            case ui::HudAction::Kind::HeroSelectGroup: {  // §2.3 tactical double click: the container, else every own object
                if (screen_ != Screen::Battle || !bview_.active() || a.unit == 0) break;  // of the hero's type; then centre
                std::vector<uint32_t> ids = a.ids;
                const render::SnapshotObject* hero = nullptr;
                for (const render::SnapshotObject& o : bview_.frame().objects)
                    if (static_cast<uint32_t>(o.id) == a.unit) hero = &o;
                if (!hero) break;
                if (ids.empty() || ids.front() == commands::kNoObject) {
                    ids.clear();
                    for (const render::SnapshotObject& o : bview_.frame().objects)
                        if (o.type == hero->type && o.owner == local_player()) ids.push_back(static_cast<uint32_t>(o.id));
                }
                if (!ids.empty()) submit(commands::ev::select(ids));
                bview_.camera().jump_to({hero->position.x, hero->position.y, hero->position.z});
                break;
            }
            case ui::HudAction::Kind::HeroFocusPlanet:  // §2.2 galactic single click: centre on the hero's planet, select it
            case ui::HudAction::Kind::HeroZoomPlanet: {  // §2.2 double click: zoom in (state 0 / 2) / out (1 / 3)
                const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
                if (!g || screen_ != Screen::Galactic || a.ids.empty() || story_controls_locked()) break;
                const PlanetIndex pi = planet_index(*g, a.ids.front());
                if (pi == kInvalidPlanet) break;
                if (a.kind == ui::HudAction::Kind::HeroFocusPlanet) {
                    const Vec3 c = g->planets()[pi].position;
                    gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    selected_planet_ = a.ids.front();  // the hero's fleet is not selected, no panel opens
                    break;
                }
                if (selected_planet_ == 0) break;
                if (!gview_.camera().zoomed()) {
                    for (const ui::PickSphere& s : gview_.pick_items())
                        if (s.kind == ui::PickKind::Planet && s.id == selected_planet_) {
                            gview_.camera().zoom_to_planet(s.center, s.radius);
                            zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
                        }
                } else if (planet_map_open_) {
                    planet_map_open_ = false;  // the advanced planet map closes instead of zooming out
                } else {
                    gview_.camera().zoom_out();
                }
                break;
            }
            case ui::HudAction::Kind::PanelClose:
                for (ui::HudPanel* p : panels())
                    for (const std::string& n : p->shell_names())
                        if (n == a.name && session_) {
                            session_->close_special_dialog();
                            panel_preview_ = 0;
                        }
                break;
            case ui::HudAction::Kind::Button:
                // OI host: game speed (UI.md §2.3 "play/pause and fast forward = game speed"): b_fast_forward[_t] toggles
                // single-player fast mode (GameSession::set_fast_forward; wall-clock pacing only).
                // The HUD's options button (b_option_t / b_option_g) opens the in-game menu, which pauses the sim.
                if (a.name == "b_option_t" || a.name == "b_option_g") {
                    if (session_ && !ui_.front.pause_menu_open()) {
                        paused_ = true;
                        ui_.front.show_pause_menu();
                    }
                    break;
                }
                // P1_DOUBLE_CLICK_MOVE.md §2.1: the attack-move / waypoint / guard order buttons (c_button01 / 03 / 05) arm the
                // one-shot move toggles of the next move order.
                if (screen_ == Screen::Battle && (iequal(a.name, "c_button01") || iequal(a.name, "c_button03") || iequal(a.name, "c_button05"))) {
                    orders_.arm_move_toggle(iequal(a.name, "c_button01")   ? ui::MoveToggle::AttackMove
                                            : iequal(a.name, "c_button03") ? ui::MoveToggle::Waypoint
                                                                           : ui::MoveToggle::GuardMove);
                    break;
                }
                if (a.name == "advanced_planetary") {  // GALACTIC_DIALOGS §4: "Base Layout" enters / leaves the planet map
                    planet_map_open_ = !planet_map_open_ && selected_planet_ != 0 && gview_.camera().zoomed();
                    break;
                }
                if (a.name == "b_story_arc_g") {  // GALACTIC_DIALOGS §2.1: the holocron button opens the story summary
                    open_story_summary();
                    break;
                }
                if (a.name == "b_fast_forward" || a.name == "b_fast_forward_t") {
                    if (session_) {
                        session_->set_fast_forward(!session_->fast_forward());
                        ++fast_toggles_;
                    }
                    break;
                }
                // OI host: the retail per-planet ability button is CommandBarComponents' g_special_ability (TextButton
                // beside the planet, one row per hero ability, GalacticHud): its HudAction carries the hero id in `unit`
                // -> SpecialAbility kind 1 for that hero at the selected planet; the sim's SpecialDialog then opens the
                // panel. (The P4 stand-in buttons - any component named market / sabotage / corrupt / hack activating the
                // first special unit - are gone: the retail rows are the only way in.)
                if (a.name == "g_special_ability") activate_special_here(a.unit);
                break;
            // P4 host: the panels' answers (SpecialAbility kinds 2 / 4 / 5 / 6 / 7 through the command stream); the preview
            // panel of the smoke sends nothing. OI host: slice rows come from the SlicePanel (unit = type).
            case ui::HudAction::Kind::BlackMarketBuy:
                if (session_ && !panel_preview_) session_->buy_black_market(a.unit);
                break;
            case ui::HudAction::Kind::SliceSelect:
                if (session_ && !panel_preview_) session_->slice_tech(a.unit);
                break;
            case ui::HudAction::Kind::SabotageTarget:
                if (session_ && !panel_preview_) session_->sabotage_structure(a.unit);
                break;
            case ui::HudAction::Kind::HackConfirm:
                if (session_ && !panel_preview_) session_->hack_super_weapon();
                break;
            case ui::HudAction::Kind::CorruptionChoice:
                if (session_ && !panel_preview_ && a.value >= 0 && a.value < 8) session_->corrupt_planet(static_cast<int8_t>(a.value));
                break;
            default: break;  // targeting modes, reinforcement drops
        }
    }
}

void App::galactic_input() {
    const uint32_t w = targets_.width, h = targets_.height;
    {  // P1_CAMERA_INPUT.md §1: the scroll clamp box = the union of the planets' spheres (centre +- radius)
        std::vector<ui::Vec3> centres;
        std::vector<float> half;
        for (const ui::PickSphere& s : gview_.pick_items())
            if (s.kind == ui::PickKind::Planet) {
                centres.push_back(s.center);
                half.push_back(s.radius);
            }
        if (!centres.empty()) gview_.set_camera_bounds(ui::galactic_scroll_box(centres, half));
    }
    if (!opt_.hidden || !opt_.smoke.empty()) gview_.camera().update(input_, ui_.bindings, dt_);
    gview_.camera().step(dt_);
    const ui::CameraParams cp = gview_.camera_params(w, h);
    ui::OrderContext ctx;
    ctx.mode = ui::UiMode::Galactic;
    ctx.local_player = local_player();
    ctx.view = ui::make_view(cp, w, h);
    const std::vector<ui::PickSphere> items = gview_.pick_items();
    // P1_CAMERA_INPUT.md §5 wheel up: the hovered planet (or the planet of the hovered fleet) becomes the selected one,
    // then the camera zooms onto the selected planet (the planet panel opens with the zoom).
    if (gview_.camera().take_zoom_in_request() && !story_controls_locked()) {
        if (const auto hov = ui::pick_spheres(items, ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()))) {
            const ui::PickSphere& s = items[*hov];
            if (s.kind == ui::PickKind::Planet) selected_planet_ = s.id;
            else if (const uint32_t at = gview_.fleet_planet_id(s.id); at != 0) selected_planet_ = at;
        }
        if (selected_planet_ != 0 && !gview_.camera().zoomed())
            for (const ui::PickSphere& s : items)
                if (s.kind == ui::PickKind::Planet && s.id == selected_planet_) {
                    gview_.camera().zoom_to_planet(s.center, s.radius);
                    zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
                }
    }
    // OI host (playtest "fleets can't move between planets") [port]: the translator picks the nearest sphere, so a fleet
    // orbiting inside / in front of its planet's sphere lost the left click to the planet, and a right click on the fleet
    // icons around the destination hit a fleet (no order). The host hands it a per-click list instead: left click - the
    // fleets alone when the ray hits one; right click - the planets plus every orbiting fleet re-labelled as its planet.
    // Left drag from an own fleet onto a planet (the original drag-to-move) is a select + move as well.
    const ui::ButtonState& lb = input_.button(ui::Button::Left);
    const ui::ButtonState& rb = input_.button(ui::Button::Right);
    std::vector<ui::PickSphere> fleets_only, as_planets;
    for (const ui::PickSphere& s : items) {
        if (s.kind == ui::PickKind::Fleet) fleets_only.push_back(s);
        // GRC 1.1: only planets with the raw revealed byte are move targets (right click, drag-to-move).
        if (s.kind == ui::PickKind::Planet && !session_->move_target_revealed(s.id)) continue;
        if (s.kind == ui::PickKind::Planet) {
            as_planets.push_back(s);
        } else if (const uint32_t at = gview_.fleet_planet_id(s.id); at != 0 && session_->move_target_revealed(at)) {
            ui::PickSphere p = s;
            p.id = at;
            p.kind = ui::PickKind::Planet;
            for (const ui::PickSphere& q : items)
                if (q.kind == ui::PickKind::Planet && q.id == at) p.owner = q.owner;
            as_planets.push_back(p);
        }
    }
    const auto mouse_ray = ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y());
    if (rb.click) ctx.galactic_items = as_planets;
    else if (lb.click && ui::pick_spheres(fleets_only, mouse_ray)) ctx.galactic_items = fleets_only;
    else ctx.galactic_items = items;
    orders_.update(input_, ui_.bindings, ctx);
    if (lb.drag_ended) {
        const auto from = ui::pick_spheres(fleets_only, ui::pixel_ray(ctx.view, lb.drag.x0, lb.drag.y0));
        const auto to = ui::pick_spheres(as_planets, ui::pixel_ray(ctx.view, lb.drag.x1, lb.drag.y1));
        if (from && to && fleets_only[*from].owner == ctx.local_player && as_planets[*to].id != gview_.fleet_planet_id(fleets_only[*from].id)) {
            orders_.set_selection({fleets_only[*from].id});
            drag_moves_.push_back({fleets_only[*from].id, as_planets[*to].id});
        }
    }
    // Planet selection (a left click on a planet; the fleet organiser opens for it) [port].
    if (input_.button(ui::Button::Left).click) {
        const auto hit = ui::pick_spheres(items, ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()));
        if (hit && items[*hit].kind == ui::PickKind::Planet) selected_planet_ = items[*hit].id;
    }
    // A left click on nothing (P1_CAMERA_INPUT.md §5): besides Select{}, the planet selection clears while not zoomed in.
    if (orders_.take_planet_deselect() && gview_.camera().zoom_state() == 0) selected_planet_ = 0;
    orders_.take_move_acks();  // no ground moves in galactic
    // A fleet selection selects the planet it orbits (the organiser's planet).
    {
        const uint32_t sel = orders_.selection().empty() ? 0 : orders_.selection().front();
        if (sel != last_fleet_sel_)
            if (const uint32_t at = sel ? gview_.fleet_planet_id(sel) : 0; at != 0) selected_planet_ = at;
        last_fleet_sel_ = sel;
    }
    if (const auto z = orders_.take_planet_zoom())
        for (const ui::PickSphere& s : items)
            if (s.id == *z) {
                gview_.camera().zoom_to_planet(s.center, s.radius);
                zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
            }
    std::vector<commands::EventData> cmds;
    for (ui::UiOrder& o : orders_.take_orders()) cmds.push_back(std::move(o.event));
    for (const auto& [fleet, planet] : drag_moves_) {
        cmds.push_back(commands::ev::select({fleet}));
        cmds.push_back(commands::ev::move_object_to_object(fleet, planet));
    }
    drag_moves_.clear();
    std::vector<ui::HotkeyEvent> hk;
    hotkeys_.update(input_, ui_.hotkeys, orders_.selection(), now_s_, cmds, hk);
    std::vector<ui::HudAction> acts;
    {
        const size_t before = cmds.size();
        const bool was_dragging = fleet_drag_active_;
        ui_.galactic_hud.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
        // Fleet drag sounds: GUI_Drag_Fleet when a fleet / unit drag starts, GUI_Drop_Fleet when its drop issued an order.
        const ui::GalacticDrag& d = ui_.galactic_hud.drag();
        fleet_drag_active_ = d.active;
        if (d.active && !was_dragging) audio_.ui("GUI_Drag_Fleet");
        if (!d.active && was_dragging && cmds.size() > before) audio_.ui("GUI_Drop_Fleet");
    }
    // L1/S4 wave 1: STORY_CLICK_GUI - a left click on a HUD component that is not disabled (the HudButtons rule: hidden /
    // disabled components never click), name = its CommandBarComponents name (b_Create_NN, b_zoom, b_planet_tab, ...).
    for (const ui::HudPointerEvent& e : routed_.hud)
        if (e.kind == ui::HudPointerEvent::Kind::Click && e.button == ui::Button::Left && e.component >= 0 &&
            static_cast<size_t>(e.component) < ui_.components.size() &&
            ui_.galactic_hud.buttons().visual(static_cast<uint32_t>(e.component)) != ui::ButtonVisual::Disabled)
            story_ui_.click_gui(ui_.components[static_cast<size_t>(e.component)].name);
    for (ui::HudPanel* p : panels())
        if (p->open()) {
            if (p == &market_) market_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &slice_) slice_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts, input_.wheel());
            else if (p == &sabotage_) sabotage_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &corruption_) corruption_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &hack_) hack_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
        }
    for (const ui::HotkeyEvent& e : hk) {
        // FIXPASS_GALACTIC §4.2 (capture C2): the advisor key (Numpad -) opens the Droid Log, as the dome and "?" do.
        if (e.slot == ui::hk::kHolocron || e.slot == ui::hk::kDroidAdvisor) open_holocron(ui::InfoPage::DroidLog);
    }
    handle_hud_actions(acts);
    route_commands(cmds);
}

void App::battle_input() {
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    if (!tm || !bview_.active()) return;
    const uint32_t w = targets_.width, h = targets_.height;
    if (!opt_.hidden || !opt_.smoke.empty()) bview_.camera().update(input_, ui_.bindings, dt_);
    const ui::CameraParams cp = bview_.camera_params(w, h);
    ui::OrderContext ctx;
    ctx.mode = bview_.space() ? ui::UiMode::Space : ui::UiMode::Land;
    ctx.local_player = local_player();
    ctx.view = ui::make_view(cp, w, h);
    ctx.trees = &tm->trees();  // const reads between ticks (P1 picking contract)
    if (!bview_.space() && bview_.scene()) {
        const terrain::MapData* m = &bview_.scene()->map();
        ctx.ground_height = [m](float x, float y) { return m->terrain.sample_height(x, y); };
    }
    const objects::ObjectStore* st = &tm->primary();
    ctx.type_of = [st](uint32_t id) {
        const objects::ObjectHandle hh = st->find(static_cast<objects::ObjectId>(id));
        return st->exists(hh) ? static_cast<uint32_t>(st->type(hh)) : 0xFFFFFFFFu;
    };
    std::vector<commands::EventData> cmds;
    // Reinforcement drop (TacticalHud BeginReinforcement): the next world left click is the landing point (ground plane
    // z = 0, land: the terrain height there [port]); a right click cancels. The click is not an order.
    bool drop_click = false;
    if (reinforce_unit_ != 0) {
        // The release after a right double press is swallowed (P1_DOUBLE_CLICK_MOVE.md §1.1): it does not cancel.
        if (input_.button(ui::Button::Right).click && !input_.button(ui::Button::Right).in_double_press) {
            reinforce_unit_ = 0;
            drop_click = true;
        } else if (input_.button(ui::Button::Left).click) {
            ui::Vec3 p{};
            if (ui::ray_plane_z(ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()), 0.0f, p)) {
                if (ctx.ground_height) p.z = ctx.ground_height(p.x, p.y);
                cmds.push_back(ui::reinforce_event(reinforce_unit_, {p.x, p.y, p.z}));
                ++reinforce_drops_;
            }
            reinforce_unit_ = 0;
            drop_click = true;
        }
    }
    // Int-final: the space reinforcement overlay (R5 fog palette 3) while a space drop point is being picked.
    bview_.set_space_reinforcement_overlay(bview_.space() && reinforce_unit_ != 0);
    // Targeted-ability pick (TargetingController, before the order translator: a frame whose click it used is not an
    // order). The click becomes the ability command through the command stream (UI.md §4.1; cmds -> route_commands).
    bool targeted = false;
    if (!drop_click && targeting_.active()) {
        std::vector<ui::UiOrder> picks;
        targeted = targeting_.update(target_input_, ui_.bindings, ctx, picks);
        for (ui::UiOrder& o : picks) {
            o.event.set_queued(o.queued);
            cmds.push_back(std::move(o.event));
        }
    }
    if (!drop_click && !targeted) orders_.update(input_, ui_.bindings, ctx);
    // Orders carry their queued flag (shift: queueable actions) into the command stream; voice per command (UiOrder::voice).
    std::vector<uint8_t> voice(cmds.size(), 1);
    for (ui::UiOrder& o : orders_.take_orders()) {
        o.event.set_queued(o.queued);
        cmds.push_back(std::move(o.event));
        voice.push_back(o.voice ? 1 : 0);
    }
    for (const ui::MoveAck& a : orders_.take_move_acks()) move_ack(a);  // P1_DOUBLE_CLICK_MOVE.md §2.3
    if (!drop_click && reticle_orders(ctx, cmds)) voice.assign(cmds.size(), 1);  // HUD_GAPS §1.6 (orders rewritten)
    if (hud_hit_debug_) {
        const int32_t px = hud_hit_px_ >= 0 ? hud_hit_px_ : input_.mouse_x(), py = hud_hit_px_ >= 0 ? hud_hit_py_ : input_.mouse_y();
        hud_hit_dump("tactical", ui_.tactical_hud.hit_test(px, py), px, py);
    }
    std::vector<ui::HotkeyEvent> hk;
    hotkeys_.update(input_, ui_.hotkeys, orders_.selection(), now_s_, cmds, hk);
    // Camera hotkeys (P1): group / selection / home focus points from the battle snapshot.
    const render::InterpolatedFrame& fr = bview_.frame();
    auto centroid = [&fr](const std::function<bool(const render::SnapshotObject&)>& want) -> std::optional<ui::Vec3> {
        double x = 0, y = 0;
        uint32_t n = 0;
        for (const render::SnapshotObject& o : fr.objects)
            if (want(o)) {
                x += o.position.x;
                y += o.position.y;
                ++n;
            }
        if (!n) return std::nullopt;
        return ui::Vec3{static_cast<float>(x / n), static_cast<float>(y / n), 0.0f};
    };
    ui::CameraFocus focus;
    const std::vector<uint32_t> sel = orders_.selection();
    const int32_t me = local_player();
    focus.selection = [&] { return centroid([&](const render::SnapshotObject& o) { return std::binary_search(sel.begin(), sel.end(), static_cast<uint32_t>(o.id)); }); };
    focus.home = [&] { return centroid([me](const render::SnapshotObject& o) { return o.owner == me; }); };
    // Control groups live in the sim (SimHost::control_group, assigned / recalled by ControlGroup commands): const read.
    const sim::SimHost& host = session_->host();
    focus.group = [&](int8_t g) -> std::optional<ui::Vec3> {
        if (g < 0 || g > 9) return std::nullopt;
        const std::vector<objects::ObjectId>& ids = host.control_group(static_cast<uint8_t>(me), static_cast<uint8_t>(g));
        return centroid([&ids](const render::SnapshotObject& o) { return std::binary_search(ids.begin(), ids.end(), o.id); });
    };
    ui::apply_camera_hotkeys(hk, bview_.camera(), focus);
    // P1_DOUBLE_CLICK_MOVE.md §2.1: the attack-move (T), guard (G) and waypoint hotkeys arm the next move order's toggle.
    for (const ui::HotkeyEvent& e : hk) {
        if (e.slot == ui::hk::kAttackMove) orders_.arm_move_toggle(ui::MoveToggle::AttackMove);
        else if (e.slot == ui::hk::kGuard) orders_.arm_move_toggle(ui::MoveToggle::GuardMove);
        else if (e.slot == ui::hk::kWaypoint) orders_.arm_move_toggle(ui::MoveToggle::Waypoint);
        else if (e.slot == ui::hk::kDroidAdvisor) open_holocron(ui::InfoPage::DroidLog);  // capture C2 (no galaxy in a skirmish: nothing)
    }
    std::vector<ui::HudAction> acts;
    ui_.tactical_hud.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
    // L1/S4 wave 1 (tactical): STORY_CLICK_GUI to the tactical plot set - the galactic rule (left click on a component that
    // is not disabled, name = its CommandBarComponents name: b_reinforcement, b_special_weapon, ...) on the tactical HUD.
    story_ui_.set_battle(tactical_story());
    if (story_ui_.battle())
        for (const ui::HudPointerEvent& e : routed_.hud)
            if (e.kind == ui::HudPointerEvent::Kind::Click && e.button == ui::Button::Left && e.component >= 0 &&
                static_cast<size_t>(e.component) < ui_.components.size() &&
                ui_.tactical_hud.buttons().visual(static_cast<uint32_t>(e.component)) != ui::ButtonVisual::Disabled)
                story_ui_.click_gui(ui_.components[static_cast<size_t>(e.component)].name);
    if (hero_panel_.open()) hero_panel_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
    handle_hud_actions(acts);
    voice.resize(cmds.size(), 1);
    route_commands(cmds, voice);
}

// P1_DOUBLE_CLICK_MOVE.md §2.3 (`0x418b0`): the particle by priority attack > guard > double > normal (a double first kills
// the nearest live normal ack silently), scaled GUI_Move_Acknowledge_Scale_Land / _Space; the radar ping
// Default_Attack_Click / Default_Click, a double removing the playing Default_Click and playing Default_Double_Click.
void App::move_ack(const ui::MoveAck& a) {
    ++move_acks_;
    if (a.kill_nearest_move_ack() && bview_.kill_nearest_move_ack(a.point)) ++move_ack_kills_;
    const ui::AckEffect fx = a.effect();
    if (bview_.spawn_move_ack(ack_fx_[static_cast<size_t>(fx)], a.point, a.space ? ack_scale_space_ : ack_scale_land_,
                              fx == ui::AckEffect::Move))
        ++move_acks_drawn_;
    if (a.remove_click_radar()) {
        const std::string& click = ack_radar_[static_cast<size_t>(ui::AckRadar::Click)];
        std::vector<uint32_t> gone;
        for (const ui::RadarEvent& e : radar_events_.items())
            if (iequal(e.type, click)) gone.push_back(e.id);
        for (const uint32_t id : gone) radar_events_.remove(id);
    }
    const std::string& radar = ack_radar_[static_cast<size_t>(a.radar())];
    if (!radar.empty()) radar_events_.add(radar_settings_, radar, a.point.x, a.point.y);
}

ui::CameraTuning App::option_tuning(ui::CameraTuning t) const {
    if (!live_input()) return t;  // smoke / replay runs: the defaults (machine and profile independent)
    t.scroll_speed_option = ui::scroll_speed_option_from_slider(gameplay_.scroll_speed);  // IDC_SCROLL_SPEED_SLIDER
    t.windowed = true;  // [port] the game window is always windowed (no exclusive full screen)
    t.absolute_scrolling = gameplay_.absolute_scroll;
    t.right_button_scroll = gameplay_.right_button_scroll;
    t.both_buttons_rotate = gameplay_.both_buttons_rotate;
    return t;
}

void App::apply_camera_tuning() {
    gview_.camera().set_tuning(option_tuning(gview_.camera().tuning()));
    if (bview_.active()) bview_.camera().set_tuning(option_tuning(bview_.camera().tuning()));
}
void App::handle_menu_actions(const std::vector<ui::MenuAction>& acts) {
    using K = ui::MenuAction::Kind;
    for (const ui::MenuAction& a : acts) {
        std::printf("menu: %s %s %s\n", ui::menu_action_name(a.kind), a.campaign.c_str(), a.map.c_str());
        switch (a.kind) {
            case K::StartCampaign: {
                SessionSpec s;
                s.kind = SessionKind::Campaign;
                s.campaign = a.campaign;
                s.faction = a.faction;
                s.ask_battle_choice = true;  // human conflicts: Fight / Auto-resolve / Retreat message box
                s.difficulty = a.difficulty;
                s.seed = opt_.seed;
                s.battle_max_frames = opt_.battle_max_frames;
                s.battle_end_hold_frames = kBattleEndHoldFrames;
                if (!start_session(s)) {
                    notes_.push_back("campaign " + a.campaign + " did not start");
                    open_main_menu();
                }
                break;
            }
            case K::StartSkirmish: {
                SessionSpec s;
                s.kind = SessionKind::Skirmish;
                s.skirmish_map = a.map;
                s.skirmish.human_faction = a.factions.size() > 0 && !a.factions[0].empty() ? a.factions[0] : "Rebel";
                s.skirmish.ai_faction = a.factions.size() > 1 && !a.factions[1].empty() ? a.factions[1] : "Empire";
                if (iequal(s.skirmish.ai_faction, s.skirmish.human_faction))  // [port] the AI needs a hostile faction
                    s.skirmish.ai_faction = iequal(s.skirmish.human_faction, "Empire") ? "Rebel" : "Empire";
                s.seed = opt_.seed;
                s.battle_max_frames = opt_.battle_max_frames;
                s.battle_end_hold_frames = kBattleEndHoldFrames;
                open_buy_units(s);  // the skirmish starts from the buy-units screen
                break;
            }
            case K::ApplyCamera:  // [port: Gabriel] FOV settings + queue modifier
                // The tactical FOV replaces TacticalCameras.xml's only once the player moved it off the default.
                if (a.camera.tactical_fov_deg != camera_.tactical_fov_deg) tactical_fov_set_ = true;
                camera_ = a.camera;
                gview_.camera().set_fov_deg(camera_.galactic_fov_deg);
                bview_.camera().set_fov_override(tactical_fov_set_ ? camera_.tactical_fov_deg : 0.0f);
                sync_control_bindings(ui_.hotkeys, camera_, ui_.bindings);  // OI host: hotkeys + queue modifier
                break;
            case K::ApplyGameplay:  // HUD_GAPS §2.2: IDC_DELAY_TOOLTIPS -> Long_Encyclopedia_Delay
                gameplay_ = a.gameplay;
                ency_cfg_.delay_tooltips = gameplay_.delay_tooltips;
                ency_.set_config(ency_cfg_);
                apply_camera_tuning();  // P1_CAMERA_INPUT.md §3.1 mouse / scroll options
                break;
            case K::SaveGame: save_game_as(a.save); break;          // N1 / t09
            case K::LoadGame: load_saved_game(a.save); break;
            case K::DeleteSave: delete_saved_game(a.save); break;
            case K::ResumeGame: paused_ = false; break;
            case K::QuitToMainMenu:
                paused_ = false;
                end_session();
                screen_ = Screen::Menu;
                open_main_menu();
                break;
            case K::ExitGame: screen_ = Screen::Quit; break;
            case K::PreviewAudio:
            case K::ApplyAudio: audio_.set_volumes(a.audio.master, a.audio.sfx, a.audio.speech, a.audio.music); break;
            case K::ApplyVideo:  // the chosen resolution resizes the window (the resize path recreates swapchain + targets)
                ++video_applies_;
                hardware_mouse_ = a.video.hardware_mouse;
                vsync_mode_ = a.video.vsync_mode;
                set_present_mode(!a.video.vsync ? rhi::PresentMode::Immediate
                                 : vsync_mode_ == ui::VsyncMode::Mailbox ? rhi::PresentMode::Mailbox : rhi::PresentMode::Fifo);
                opt_.frame_cap = a.video.frame_cap;
                if (a.video.resolution >= 0 && static_cast<size_t>(a.video.resolution) < display_modes_.size()) {
                    // The list item is a full mode: the window size and the monitor's refresh rate (game_display.hpp).
                    const DisplayMode m = display_modes_[static_cast<size_t>(a.video.resolution)];
                    if (m.width != win_.width() || m.height != win_.height() || (m.hz && m.hz != refresh_hz_)) {
                        std::string line;
                        apply_display_mode(win_.native_handle(), m, &line);
                        std::printf("video: %s\n", line.c_str());
                    }
                }
                update_display_rate(true);
                // t12: tried now, stored on the Yes answer of "keep these settings?" (or as the old mode on No / timeout).
                {
                    const int32_t old_detail = video_profile_.detail;
                    if (update_video_profile(video_profile_, a.video, display_modes_)) {
                        adv_video_.anti_aliasing = video_profile_.advanced.anti_aliasing;  // t19 / D9
                        // t11 [M]: a moved overall Detail slider is a preset for the seven advanced detail sliders.
                        if (video_profile_.detail != old_detail) {
                            adv_video_ = detail_preset(adv_video_, video_profile_.detail);
                            video_profile_.advanced = adv_video_;
                        }
                        save_video_options();
                    }
                }
                break;
            case K::ApplyAdvancedVideo:  // TAA / AO go into the next frame's RecordOptions; anisotropy latches next frame
                adv_video_ = a.advanced;
                gpu_.renderer.set_anisotropy(static_cast<uint32_t>(adv_video_.anisotropy));
                std::printf("video: anti-aliasing %s, ambient occlusion %s, anisotropy %dx requested\n",
                            adv_video_.anti_aliasing == ui::AntiAliasing::Taa ? "TAA" : "off", adv_video_.ambient_occlusion ? "on" : "off",
                            adv_video_.anisotropy);
                video_profile_.advanced = adv_video_;
                sync_detail(video_profile_);  // t43: the Detail slider follows
                video_profile_.anti_alias = ui::anti_alias_slider_for(adv_video_.anti_aliasing, video_profile_.anti_alias);  // t19 / D9
                save_video_options();
                break;
            default: break;
        }
    }
}

// ------------------------------------------------------------------------------------------------ screens / panels
namespace {
// HudDrawList b's quads appended to a (every layer / sub-list, in order).
void merge_lists(ui::HudDrawList& a, const ui::HudDrawList& b) {
    for (uint32_t l = 0; l < ui::kHudLayers; ++l)
        for (uint32_t s = 0; s < ui::kHudSubLists; ++s)
            for (const ui::HudQuad& q : b.items(l, static_cast<ui::HudList>(s))) a.add(l, static_cast<ui::HudList>(s), q);
}
// Display name of planet row `p` (its type's Text_ID, else the row name).
std::u32string planet_label(const GameData& d, const ui::TextDatabase& text, const galactic::GalacticSim& g, PlanetIndex p) {
    if (p >= g.planets().size()) return {};
    const uint32_t t = g.planets()[p].type;
    if (t < d.types.size()) return type_label(d, text, static_cast<objects::TypeIndex>(t));
    return ui::utf8_to_utf32(p < g.map().planets.name.size() ? g.map().planets.name[p] : std::string());
}
}  // namespace

uint32_t App::display_planet_type(const galactic::GalacticSim& g, PlanetIndex p) const {
    const uint32_t own = p < g.planets().size() ? g.planets()[p].type : galactic::kNoIndex;
    if (!session_ || session_->spec().kind != SessionKind::Skirmish) return own;
    const uint32_t t = g.rules().find_type(skirmish_map_planet_type(g.rules(), session_->spec().skirmish_map));
    return t != galactic::kNoIndex ? t : own;
}

// [port: Gabriel] the window's monitor refresh rate (re-read every 2 s at 60 Hz: the window may move to another
// monitor) and the frame rate cap it sets for "Display refresh rate". Live play only: smoke / hidden runs are uncapped.
void App::update_display_rate(bool force) {
    if (!force && frames_ - refresh_checked_frame_ < 120) return;
    refresh_checked_frame_ = frames_;
    const uint32_t hz = monitor_refresh_hz(win_.native_handle());
    if (hz != refresh_hz_) std::printf("display: %u Hz\n", hz);
    refresh_hz_ = hz;
    const int32_t cap = opt_.frame_cap == ui::kFrameCapDisplay ? static_cast<int32_t>(refresh_hz_) : opt_.frame_cap;
    limiter_.set_fps(live_input() && cap > 0 ? static_cast<uint32_t>(cap) : 0u);
}

// A new swap mode takes effect on a swapchain recreate (same size).
void App::set_present_mode(rhi::PresentMode m) {
    if (m == opt_.mode) return;
    opt_.mode = m;
    sc_.set_present_mode(m);
    gpu_.ring.wait_idle();
    sc_.recreate(win_.width(), win_.height());
    const VkPresentModeKHR pm = sc_.present_mode();
    std::printf("video: present mode %s\n", pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
}

// [port: Gabriel] render interpolation between the last two sim snapshots (InterpMode::Smooth), so motion stays smooth
// at any refresh rate / frame rate above the tick rate: alpha = time since the last tick over the last tick interval
// (clamped to 1..2 pacing periods). Smoke and hidden runs draw the current snapshot (alpha 1, the original behaviour).
float App::render_alpha() {
    if (!live_input() || !session_ || last_tick_ms_ == prev_tick_ms_) return 1.0f;
    const uint32_t pace = session_->host().pacing().period_ms();
    const uint32_t period = std::clamp(last_tick_ms_ - prev_tick_ms_, pace, 2u * pace);
    return render::tick_alpha(static_cast<uint32_t>(ms_since(t0_)), last_tick_ms_, period);
}

// Skirmish buy-units screen (IDD_SKIRMISH_BUY_UNITS_DIALOG): [port] 10000 credits, the human faction's buildable space
// units (land map: ground units) by cost; bought units replace the default fleet, none bought = the default fleet.
void App::open_buy_units(const SessionSpec& spec) {
    pending_skirmish_ = spec;
    bought_.clear();
    buy_ = {};
    buy_.credits = opt_.smoke == "skirmish" ? 20000 : 10000;  // [smoke] room for a fleet larger than the space unit cap
    const bool land = spec.skirmish_map.find("_LAND") != std::string::npos;
    const uint32_t fac = data_.rules.find_faction(spec.skirmish.human_faction);
    for (uint32_t t = 0; t < data_.rules.types.size() && buy_.units.size() < 24; ++t) {
        const galactic::TypeInfo& ti = data_.rules.types[t];
        const bool kind_ok = land ? ti.kind == galactic::UnitKind::GroundUnit : ti.kind == galactic::UnitKind::SpaceUnit;
        if (!kind_ok || ti.build_cost <= 0 || data_.types.get_bool(t, "Is_Named_Hero")) continue;
        if (!ti.affiliation.empty() && std::find(ti.affiliation.begin(), ti.affiliation.end(), fac) == ti.affiliation.end()) continue;
        ui::BuyUnitEntry e;
        e.type = ti.name;
        e.name = type_label(data_, ui_.text, t);
        e.cost = ti.build_cost;
        e.icon = t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string();
        buy_.units.push_back(std::move(e));
    }
    std::stable_sort(buy_.units.begin(), buy_.units.end(), [](const ui::BuyUnitEntry& a, const ui::BuyUnitEntry& b) { return a.cost < b.cost; });
    if (!info_.show_buy_units(buy_)) {  // no dialog in the script: start with the defaults
        if (!start_session(spec)) open_main_menu();
    }
}

void App::handle_screen_actions(const std::vector<ui::ScreenAction>& acts) {
    using K = ui::ScreenAction::Kind;
    for (const ui::ScreenAction& a : acts) {
        switch (a.kind) {
            case K::BuyUnit:
            case K::SellUnit:
                for (ui::BuyUnitEntry& u : buy_.units)
                    if (u.type == a.map) {
                        if (a.kind == K::BuyUnit && buy_.credits >= u.cost) {
                            buy_.credits -= u.cost;
                            ++u.owned;
                            ++bought_[u.type];
                        } else if (a.kind == K::SellUnit && u.owned > 0) {
                            buy_.credits += u.cost;
                            --u.owned;
                            if (--bought_[u.type] == 0) bought_.erase(u.type);
                        }
                    }
                for (ui::BuyUnitEntry& u : buy_.units) u.can_buy = buy_.credits >= u.cost;
                info_.set_buy_units(buy_);
                break;
            case K::SellAllUnits:
                for (ui::BuyUnitEntry& u : buy_.units) {
                    buy_.credits += static_cast<int64_t>(u.cost) * u.owned;
                    u.owned = 0;
                    u.can_buy = true;
                }
                bought_.clear();
                info_.set_buy_units(buy_);
                break;
            case K::BuyUnitsDone: {
                SessionSpec s = pending_skirmish_;
                std::vector<std::string> units;
                for (const auto& [type, n] : bought_)
                    for (uint32_t k = 0; k < n; ++k) units.push_back(type);
                const bool land = s.skirmish_map.find("_LAND") != std::string::npos;
                if (!units.empty()) (land ? s.skirmish.ground_human : s.skirmish.units_human) = units;
                if (!start_session(s)) {
                    notes_.push_back("skirmish on " + s.skirmish_map + " did not start");
                    open_main_menu();
                }
                break;
            }
            case K::BuyUnitsBack: open_main_menu(); break;
            case K::StorySummaryClosed:
                story_summary_open_ = false;
                break;
            case K::BattleEndContinue:
                battle_end_open_ = false;
                story_ui_.generic("battle_end_closed");  // L1/S4 wave 1: STORY.md 4.1 step 6, GENERIC "battle_end_closed"
                if (battle_end_auto_) audio_.galactic_resume();  // the win / lose music ends with the screen
                battle_end_auto_ = false;
                if (session_ && session_->spec().kind == SessionKind::Skirmish) {
                    skirmish_ended_ = true;
                    screen_ = Screen::Menu;
                    if (opt_.smoke.empty()) session_.reset();  // smoke runs keep it for the CRC check
                    open_main_menu();
                }
                break;
            case K::MessageBoxResult:
                if (a.tag == "battle_choice") {  // [port] a scripted message-box prompt: button 1 = Auto-Resolve
                    answer_battle_prompt(a.button == 1);
                } else if (a.tag == "retry") {  // Lua Activate_Retry_Dialog: Retry restarts the session, Quit leaves it
                    if (a.button == 0 && session_) {
                        const SessionSpec spec = session_->spec();
                        if (!start_session(spec)) open_main_menu();
                    } else {
                        end_session();
                        screen_ = Screen::Menu;
                        open_main_menu();
                    }
                }
                break;
            case K::CampaignScoreClosed:  // the campaign is over: back to the main menu (smoke runs keep the session)
                screen_ = Screen::Menu;
                if (opt_.smoke.empty()) end_session();
                ui_.front.set_in_game(false);
                open_main_menu();
                break;
            case K::StorySummaryGoal:  // GALACTIC_DIALOGS §2.2: a chapter chosen in the goal list -> refill with it
                summary_chapter_ = a.button;
                open_story_summary();
                break;
            case K::StorySummaryFilter:
                summary_completed_ = a.button == 1;
                open_story_summary();  // refill
                break;
            // P4 host: both answers go through the command stream; their results come at the command phase
            // (GameSession::port_event). The auto-resolve dialog stays until its result (galactic_screens). The story
            // summary's Abandon is permanently disabled (P4_SIM_SYSTEMS §9.1).
            case K::Withdraw:
                if (session_ && session_->withdraw()) ++withdraws_;
                std::printf("auto-resolve: withdraw requested\n");
                break;
            case K::AutoResolve:  // the retreat dialog's Autoresolve (tactical)
                if (session_ && session_->auto_resolve_battle()) ++tactical_auto_resolves_;
                std::printf("battle: auto-resolve requested\n");
                break;
            default: break;  // retreat (its command was sent), planet info / story / holocron closes: nothing to do
        }
    }
}

void App::show_battle_end() {
    ui::BattleEndInfo info;
    const std::string log = session_ && session_->battles() && !session_->battles()->log().empty() ? session_->battles()->log().back() : std::string();
    // [port] winner from the battle summary ("winner <faction>,").
    std::string winner;
    if (const size_t w = log.find("winner "); w != std::string::npos) {
        const size_t e = log.find(',', w);
        winner = log.substr(w + 7, e == std::string::npos ? std::string::npos : e - w - 7);
        info.victory = iequal(winner, local_faction());
    }
    // Battle-end win / lose music, mode 16 (STRATEGIC_GAPS §7.4); a lost battle names the winner as the enemy faction.
    audio_.battle_end(info.victory, info.victory ? std::string_view() : std::string_view(winner));
    // L1/S4 wave 1 (SCRIPT_GAPS.md 4.2): the local player lost a battle that ran a tactical plot set -> STORY_MISSION_LOST
    // to the galactic story, name = that plots file (the shipped Param1 values) [inferred: the original's "monitor
    // MISSION_LOST name" source is not specified].
    if (!info.victory && !battle_plots_.empty()) story_ui_.mission_lost(battle_plots_);
    battle_plots_.clear();
    info.planet = battle_place_;  // QA U1: the localized place, never the log's map file name
    // Playtest QA D1 / V1: the ended battle's own records (the old per-frame map also kept the attract demo's losses and
    // missed the final tick's deaths).
    std::vector<game::BattleLossRecord> dead;
    if (session_ && session_->battles())
        for (const sim::TacticalBattle::RemovalRecord& r : session_->battles()->last_battle_losses()) dead.push_back({r.owner, r.object_type});
    const game::BattleKillCount kills = game::count_battle_kills(dead, local_player());  // t41: campaign score, final tick included
    tally_.kills += kills.kills;
    tally_.losses += kills.losses;
    for (const auto& [owner, types] : game::tally_battle_losses(dead))
        for (const auto& [t, n] : types) {
            ui::BattleEndUnit u;
            u.name = type_label(data_, ui_.text, t);
            u.icon = t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string();
            u.count = n;
            (owner == local_player() ? info.your_losses : info.enemy_losses).push_back(std::move(u));
        }
    info.can_save_replay = false;  // N1
    battle_end_open_ = screens_.show_battle_end(info);
}

// GALACTIC_DIALOGS §3.1 step 2: IDD_BATTLE_END_DIALOG after the local player's auto-resolve (pauses: ticks wait for
// Continue). Title TEXT_WIN_BATTLE / TEXT_LOST_BATTLE, the planet, the battle time, salvage (TEXT_SUMMARY_BOUNTY_COLLECTED),
// both sides' lost units by type (the forces recorded at the choice that are gone now; types costing 0 skipped) with their
// credit values, the lost named heroes' icons (5 per side) and the combat efficiency. Playtest 2 t13: time, bounty and
// efficiency come from game::summarize_auto_resolve (battle_summary.hpp; display-only [M] rules, no sim state touched).
void App::show_auto_resolve_end(const galactic::ConflictLogEntry& e) {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return;
    const int32_t me = local_player();
    ui::BattleEndInfo info;
    info.victory = e.winner == me;
    info.title = info.victory ? txt("TEXT_WIN_BATTLE", U"You have won the battle!") : txt("TEXT_LOST_BATTLE", U"You have lost the battle!");
    info.planet = planet_label(data_, ui_.text, *g, e.planet);
    const game::BountyTable bounty_table = game::load_bounty_table(data_.db.constants(), data_.types);
    std::vector<game::ArUnitRecord> records;
    records.reserve(auto_units_.size());
    std::map<int32_t, std::map<objects::TypeIndex, uint32_t>> lost;
    for (const AutoUnit& u : auto_units_) {
        const uint32_t t = static_cast<uint32_t>(u.type);
        const bool gone = !g->store().alive(u.h);
        game::ArUnitRecord r;
        r.owner = u.owner;
        r.destroyed = gone;
        r.cost = t < data_.rules.types.size() ? static_cast<int64_t>(data_.rules.types[t].build_cost) : 0;
        r.bounty = game::type_bounty(bounty_table, data_.types, u.type);
        records.push_back(r);
        if (!gone) continue;
        if (is_named_hero(*g, t)) {
            std::vector<std::string>& heroes = u.owner == me ? info.your_heroes : info.enemy_heroes;
            const std::string icon = t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string();
            if (!icon.empty() && heroes.size() < 5) heroes.push_back(icon);
        }
        if (r.cost <= 0) continue;  // §3.1: types costing 0 are not listed
        ++lost[u.owner][u.type];
    }
    auto_units_.clear();
    for (const auto& [owner, types] : lost)
        for (const auto& [t, n] : types) {
            ui::BattleEndUnit u;
            u.name = type_label(data_, ui_.text, t);
            u.icon = t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string();
            u.count = n;
            (owner == me ? info.your_losses : info.enemy_losses).push_back(std::move(u));
        }
    const game::ArSummary sum = game::summarize_auto_resolve(records, me, info.victory);
    info.battle_seconds = sum.battle_seconds;
    info.bounty = sum.bounty;
    info.your_loss_value = sum.loss_value[0];
    info.enemy_loss_value = sum.loss_value[1];
    info.your_efficiency = sum.efficiency[0];
    info.enemy_efficiency = sum.efficiency[1];
    info.can_save_replay = false;
    audio_.battle_end(info.victory, std::string_view());
    battle_end_open_ = screens_.show_battle_end(info);
    battle_end_auto_ = battle_end_open_;
    if (battle_end_open_) ++auto_results_;
    const bool attacker = e.attacker == me;
    std::printf("auto-resolve end: %s at %s, time %02lld:%02lld:%02lld, salvage %lld, units lost yours %u ($%lld) / enemy %u ($%lld), "
                "efficiency %d%% / %d%%, heroes lost %zu / %zu (sim combatants yours %u / enemy %u)\n",
                info.victory ? "victory" : "defeat", g->map().planets.name[e.planet].c_str(),
                static_cast<long long>(sum.battle_seconds / 3600), static_cast<long long>((sum.battle_seconds / 60) % 60),
                static_cast<long long>(sum.battle_seconds % 60), static_cast<long long>(sum.bounty), sum.units_lost[0],
                static_cast<long long>(sum.loss_value[0]), sum.units_lost[1], static_cast<long long>(sum.loss_value[1]),
                sum.efficiency[0], sum.efficiency[1], info.your_heroes.size(), info.enemy_heroes.size(),
                attacker ? e.attacker_losses : e.defender_losses, attacker ? e.defender_losses : e.attacker_losses);
}

namespace {
// UTF-32 -> UTF-8 for the console log lines.
std::string utf8_of(std::u32string_view s) {
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
}  // namespace

std::u32string App::txt(std::string_view id, std::u32string_view fallback) const {
    if (const std::u16string* s = ui_.text.find(id)) return ui::utf16_to_utf32(*s);
    return std::u32string(fallback);
}

// [port] the faction's Text_ID (FACTIONS.XML: TEXT_FACTION_REBELS / _EMPIRE / _UNDERWORLD ...), else its name.
std::u32string App::faction_label(int32_t player) const {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g || player < 0 || static_cast<size_t>(player) >= g->players().size()) return {};
    const uint32_t f = g->players()[static_cast<size_t>(player)].faction;
    if (f >= g->rules().factions.size()) return {};
    const std::string& name = g->rules().factions[f].name;
    std::string id = "TEXT_FACTION_";
    for (const char c : name) id.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (iequal(name, "Rebel")) id = "TEXT_FACTION_REBELS";
    return txt(id, ui::utf8_to_utf32(name));
}

void App::screen_line(std::u32string text, ui::Rgba8 color) {
    std::printf("screen text: %s\n", utf8_of(text).c_str());
    ++screen_line_count_;
    screen_lines_.push_back({std::move(text), color, now_s_});
    if (screen_lines_.size() > 8) screen_lines_.erase(screen_lines_.begin());
}

// ------------------------------------------------------------------------------------------------ N1 / t09 saves
std::string App::session_title() const {
    if (!session_) return {};
    const SessionSpec& s = session_->spec();
    if (s.kind == SessionKind::Skirmish) return s.skirmish_map;
    if (const data::CampaignDef* c = data_.db.find_campaign(s.campaign); c && !c->text_id.empty() && ui_.text.find(c->text_id))
        return utf8_of(txt(c->text_id, {}));
    return s.campaign;
}

// A message box with up to three lines of `detail` (the reason a save / load failed), one OK button.
void App::save_message(std::u32string caption, const std::string& detail) {
    std::printf("saves: %s: %s\n", utf8_of(caption).c_str(), detail.c_str());
    ui::MessageBoxSpec m;
    m.tag = "save_message";
    m.caption = std::move(caption);
    std::string rest = detail;
    while (!rest.empty() && m.lines.size() < 3) {
        size_t cut = rest.size();
        if (cut > 64 && m.lines.size() < 2) {
            cut = rest.rfind(' ', 64);
            if (cut == std::string::npos || cut == 0) cut = 64;
        }
        m.lines.push_back(ui::utf8_to_utf32(rest.substr(0, cut)));
        rest.erase(0, std::min(rest.size(), cut + (cut < rest.size() && rest[cut] == ' ' ? 1u : 0u)));
    }
    m.buttons = {txt("TEXT_BUTTON_OK", U"OK")};
    screens_.show_message(m);
}

void App::save_game_as(const std::string& typed) {
    if (!session_ || demo_session()) return save_message(U"The game cannot be saved now", "no game is running");
    if (saves_dir_.empty()) return save_message(txt("TEXT_SAVE_FAILED", U"Failed to save game."), "no saved games folder");
    std::string path, err;
    if (!save_game(*session_, saves_dir_, typed, session_title(), unix_now(), &path, &err)) {
        const bool refused = !session_->check_save().empty();  // cached: no second save
        save_message(refused ? std::u32string(U"The game cannot be saved now") : txt("TEXT_SAVE_FAILED", U"Failed to save game."), err);
    } else {
        ++saves_written_;
        std::printf("saves: saved %s at tick %" PRIu64 "\n", path.c_str(), session_->host().frame());
        screen_line(txt("TEXT_SAVED_OK", U"Game Saved"), ui::Rgba8{255, 255, 255, 255});
    }
    ui_.front.refresh_saves();
}

// Load from the main menu or the in-game menu: the save is rebuilt and loaded into a new session first; only when that
// worked does it replace the running session (a failed load leaves the current game / menu as it was).
bool App::load_saved_game(const std::string& name) {
    const std::string path = save_path(saves_dir_, name);
    std::string err;
    std::unique_ptr<GameSession> s = path.empty() ? nullptr : load_game(data_, *js_, path, &err);
    if (!s) {
        save_message(U"The saved game could not be loaded", path.empty() ? std::string("no save name") : err);
        return false;
    }
    attract_.stop();  // as start_session: a real game ends the attract demo
    end_session();
    session_ = std::move(s);
    recording_ = commands::CommandRecording{};
    session_->record_to(&recording_);  // record_after_load: the stream continues the save
    loaded_save_ = path;
    ++saves_loaded_;
    paused_ = false;
    session_started({});
    hotkeys_.set_paused(session_->pause_requested());  // a game saved paused comes back paused
    return true;
}

void App::delete_saved_game(const std::string& name) {
    std::string err;
    if (!delete_save(saves_dir_, name, &err)) save_message(U"The saved game could not be deleted", err);
    else std::printf("saves: deleted %s\n", name.c_str());
    ui_.front.refresh_saves();
}

// GALACTIC_DIALOGS §3.1 / BATTLE_TRANSITIONS §4.1 step 4: an auto-resolved conflict (log how = 1) the local player took
// part in, or at a planet whose fleets it can see [port: PlanetInfo::FleetPresence], posts the red result line
// TEXT_AR_{SPACE,LAND}_{WIN,LOSS}_{MAJOR,REGULAR,MINOR} (##0 = the viewpoint's faction, ##1 = planet, ##2 = its recorded
// losses; viewpoint = the local player, else the winner) and TEXT_AR_HERO_LOSS per dead named hero [port: known for the
// local player's own auto-resolve only]. The battle-end screen follows the local player's Auto-Resolve choice (never an
// overrun or an AI battle, which are not asked).
void App::auto_resolve_result(const galactic::GalacticSim& g, const galactic::ConflictLogEntry& e) {
    const int32_t me = local_player();
    const bool involved = e.attacker == me || e.defender == me;
    const bool chosen = auto_pending_ && involved && e.planet == auto_planet_;
    if (chosen) auto_pending_ = false;
    if (!involved && !g.can_see_planet_info(me, e.planet, galactic::GalacticSim::PlanetInfo::FleetPresence)) return;
    const int32_t view = involved ? me : e.winner;
    const bool view_attacker = view == e.attacker;
    const uint32_t n_own = view_attacker ? e.attacker_losses : e.defender_losses;
    const uint32_t n_other = view_attacker ? e.defender_losses : e.attacker_losses;
    const bool land = e.kind == galactic::ConflictKind::Land;
    const bool win = e.winner == view;
    const std::u32string planet = planet_label(data_, ui_.text, g, e.planet);
    screen_line(ui::auto_resolve_text(&ui_.text, !land, win, ar_result_magnitude(win, n_own, n_other), faction_label(view), planet, n_own),
                ui::kAutoResolveTextColor);
    if (chosen)
        for (const AutoUnit& u : auto_units_)
            if (!g.store().alive(u.h) && is_named_hero(g, static_cast<uint32_t>(u.type)))
                screen_line(ui::auto_resolve_hero_loss_text(&ui_.text, type_label(data_, ui_.text, u.type), planet), ui::kAutoResolveTextColor);
    if (chosen) show_auto_resolve_end(e);
}

// GALACTIC_DIALOGS §1: the battle-pending prompt for the head conflict (paused: the conflict manager holds the galactic
// pause in Vote; no timeout in single player). Feed: the planet, space / land, the title, Begin-only when the story lock
// forbids auto-resolve (§1.4; [port] no tutorial lock / player flag source yet), each side's commander icon and stack
// depth (Ships_Per_Stack); the faction pending SFX and music.
// §1.5: Begin -> Fight = conflict state 3; Auto-Resolve (two-button prompt only) -> AutoResolve = state 6. No Retreat. SP
// auto-resolve has no progress dialog (§3.1): its result comes from the conflict log (auto_resolve_result).
void App::answer_battle_prompt(bool auto_resolve) {
    if (!choice_open_) return;
    choice_open_ = false;
    const bool ar = auto_resolve && !prompt_.story_pending;
    const galactic::BattleChoice c = ar ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight;
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    const galactic::ConflictInfo* head = session_ ? session_->conflict_head() : nullptr;
    const bool sent = session_ && session_->choose_battle(c);
    if (sent && !ar && head) open_battle_load(head->planet, head->kind == galactic::ConflictKind::Space);  // QA t11
    if (sent && ar && g && head) {
        ++auto_resolves_;
        auto_pending_ = true;
        auto_planet_ = head->planet;
        auto_units_.clear();  // the forces as they stand when the auto-resolve starts (losses = the ones gone after)
        for (const int32_t side : {head->attacker, head->defender})
            for (const objects::ObjectHandle m : conflict_units(*g, *head, side)) auto_units_.push_back({side, g->store().type(m), m});
    }
    std::printf("battle choice: %s\n", ar ? "auto-resolve" : "begin");
}

void App::open_battle_prompt(const galactic::GalacticSim& g, const galactic::ConflictInfo& c) {
    prompt_ = {};
    prompt_.planet = c.planet;
    prompt_.land = c.kind == galactic::ConflictKind::Land;
    prompt_.story_pending = session_->autoresolve_locked(c.planet);
    const int32_t me = local_player();
    const int32_t enemy = c.attacker == me ? c.defender : c.attacker;
    auto icon = [this](uint32_t t) { return t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string(); };
    const uint32_t per = static_cast<uint32_t>(std::max(1, data_.db.constants().get_int("Ships_Per_Stack", 5)));
    const std::vector<objects::ObjectHandle> mine = conflict_units(g, c, me), theirs = conflict_units(g, c, enemy);
    prompt_.left = battle_force_summary(g, mine, icon, per);
    prompt_.right = battle_force_summary(g, theirs, icon, per);
    prompt_.title = planet_label(data_, ui_.text, g, c.planet) +
                    (prompt_.land ? txt("TEXT_TACTICAL_LAND_BATTLE_IS_PENDING", U": PLANETARY BATTLE IMMINENT!")
                                  : txt("TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING", U": SPACE BATTLE IMMINENT!"));
    prompt_.planet_name = planet_label(data_, ui_.text, g, c.planet);
    audio_.battle_pending(prompt_.land);
    choice_open_ = true;  // the banner (GalacticHudFrame::battle) until Begin / Auto-Resolve
    ++choices_;
    if (prompt_.story_pending) ++prompts_story_;
    std::printf("battle prompt: %s (%s, %s), yours %u units / %u stacks [%s], enemy %u units / %u stacks [%s]\n",
                utf8_of(prompt_.title).c_str(), prompt_.land ? "land" : "space", prompt_.story_pending ? "begin only" : "begin / auto-resolve",
                prompt_.left.units, prompt_.left.stacks, prompt_.left.icon.c_str(), prompt_.right.units, prompt_.right.stacks,
                prompt_.right.icon.c_str());
}

// GALACTIC_DIALOGS §2.1: a Story_Dialog event's chapter goes to the mission holocron; b_story_arc_g flashes, the faction's
// Default_Transmission_Message (TEXT_ENCYCLOPEDIA_HOLOCRON_UPDATED) is a white screen-text line and SFXEvent_Mission_Added
// plays [port: always - no shipped event sets Story_Dialog_SFX, which the story model does not read]. Story_Dialog_Popup
// opens the summary at once.
void App::add_holocron_chapter(const ui::StoryDialogInfo& info, const StoryDialogMeta& meta) {
    HolocronChapter ch;
    ch.title = info.title;
    ch.lines = info.lines;
    ch.colors = info.line_colors;
    ch.dialog = meta.dialog;
    ch.chapter = meta.chapter;
    holocron_chapters_.push_back(std::move(ch));
    ++holocron_updates_;
    holocron_flash_ = true;
    summary_chapter_ = -1;  // the summary opens at the newest chapter
    screen_line(txt("TEXT_ENCYCLOPEDIA_HOLOCRON_UPDATED", U"The mission holocron has been updated."), ui::Rgba8{255, 255, 255, 255});
    audio_.faction_sfx("SFXEvent_Mission_Added");
    std::printf("holocron: chapter %d of %s added (%zu lines)%s\n", meta.chapter, meta.dialog.c_str(), holocron_chapters_.back().lines.size(),
                meta.popup ? ", popup" : "");
    if (meta.popup) open_story_summary();
}

void App::update_radar() {
    const render::FogRenderer& fog = bview_.fog();
    if (!bview_.active()) return;
    // Space asteroid fields (RadarMap.xml Space_Asteroid_Field_Color): the battle's SpaceFieldIndex, read once per battle.
    if (!radar_fields_built_) {
        if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr) {
            radar_fields_built_ = true;
            if (bview_.space()) radar_fields_ = radar_asteroid_fields(b->space_fields().fields());
            radar_fields_dirty_ = !radar_fields_.empty();
        }
    }
    const bool fog_new = !fog.processed().empty() && fog.stats().uploads != radar_fog_uploads_;
    if (!fog_new && !radar_fields_dirty_) return;
    radar_fog_uploads_ = fog.stats().uploads;
    radar_fields_dirty_ = false;
    uint32_t w = 0, h = 0;
    std::span<const uint8_t> vis;
    if (!fog.processed().empty()) {
        w = fog.grid().width();
        h = fog.grid().height();
        if (static_cast<size_t>(w) * h != fog.processed().size()) return;
        vis = fog.processed();
    }
    ui::RadarFieldPaint paint;  // the radar's world box (build_tactical_hud's map_min / max)
    paint.fields = radar_fields_;
    if (const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr)
        if (tm->bounds().max.x - tm->bounds().min.x < 1.0e5f) {
            paint.min_x = tm->bounds().min.x;
            paint.min_y = tm->bounds().min.y;
            paint.max_x = tm->bounds().max.x;
            paint.max_y = tm->bounds().max.y;
        }
    paint.fill = radar_settings_.space_asteroid;
    paint.border = radar_settings_.space_asteroid_border;
    radar_.build(128, {}, 0, 0, vis, w, h, bview_.space() ? radar_settings_.space_fow : radar_settings_.land_fow, paint);
    if (!radar_.empty()) gpu_.hud.set_dynamic_page(ui::kHudPageRadar, radar_.width(), radar_.height(), radar_.rgba(), radar_.generation());
}

// ---- information screens fed from the sim (game_info.hpp) ----

// Galactic-map screens each frame: data-chart samples, story dialogs fired since the last frame (GALACTIC_DIALOGS §2:
// the mission holocron; IDD_STORY_DIALOG only for random-plot offers), the battle prompt (§1), auto-resolve results (§3)
// and the campaign end (campaign score).
void App::galactic_screens() {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return;
    holo_.sample(*g);
    std::vector<StoryMovie> movies;
    std::vector<StoryDialogMeta> meta;
    std::vector<ui::StoryDialogInfo> dialogs = new_story_dialogs(*session_, data_, ui_.text, story_cursor_, &movies, &meta);
    bool offer = false;  // a random-plot offer opened (its talking head plays in the dialog's portrait)
    for (size_t k = 0; k < dialogs.size(); ++k) {
        ui::StoryDialogInfo& i = dialogs[k];
        const StoryDialogMeta md = k < meta.size() ? meta[k] : StoryDialogMeta{};
        if (!md.random_plot) {
            add_holocron_chapter(i, md);
            continue;
        }
        // ui ask 11: the talking head (MOVIE looped / MOVIE_ONCE) plays in the dialog's portrait: a GUI image page named
        // after the movie, filled with the movie's frames (StoryDialogInfo::image = that name).
        if (!movies.empty() && i.image.empty()) i.image = movies.front().name;
        if (info_.show_story(i)) {
            ++story_dialogs_;
            offer = true;
        }
    }
    for (const StoryMovie& m : movies) {
        if (!offer) break;  // [port] holocron chapters play no movie of their own; MULTIMEDIA rewards drive Movie_galactic
        std::string err;
        if (advisor_movie_.open(data_, audio_, m.name, m.loop, &err)) {
            story_movie_ = true;
            story_page_ = ui_.pages.page(m.name);
            // Marked loaded at once (the movie's size) so the page loader never looks for a texture of that name.
            if (story_page_ >= 0) ui_.pages.set_loaded(static_cast<uint16_t>(story_page_), advisor_movie_.width(), advisor_movie_.height());
        } else {
            std::fprintf(stderr, "story movie %s: %s\n", m.name.c_str(), err.c_str());
        }
    }
    // The open story dialog's script clock (wall time, presentation only): lines appear at their WAITs, SFX / DIALOG cues
    // play once.
    std::vector<ui::StoryDialogCue> cues;
    info_.update_story(now_s_, &cues);
    for (const ui::StoryDialogCue& c : cues) {
        if (c.kind == ui::StoryDialogCue::Kind::Speech) audio_.speech(c.event);
        else audio_.ui(c.event);
        ++story_cues_;
    }
    if (story_movie_ && !ui_.gui.find(ui::dlg::kStory)) {  // the talking head ends with its dialog
        advisor_movie_.stop(audio_);
        gpu_.hud.clear_image_page(kAdvisorPage);
        story_movie_ = false;
        story_page_ = -1;
    }
    if (story_summary_open_ && !ui_.gui.find(ui::dlg::kStorySummary)) story_summary_open_ = false;  // closed by Esc
    if (session_->battle_choice_pending() && !choice_open_)
        if (const galactic::ConflictInfo* c = session_->conflict_head()) open_battle_prompt(*g, *c);
    if (const sim::CampaignBattles* b = session_->battles()) {
        const std::vector<galactic::ConflictLogEntry>& log = b->conflicts().log();
        while (ar_log_seen_ < log.size()) {
            const galactic::ConflictLogEntry& e = log[ar_log_seen_++];
            if (e.how == 1) auto_resolve_result(*g, e);
        }
    }
    if (session_->spec().kind == SessionKind::Campaign && !campaign_over_)
        if (const int32_t o = galactic::victory_outcome(*g, victory_rules_, local_player()); o != 0) show_campaign_end(o > 0);  // Int-verify
}

void App::show_campaign_end(bool victory) {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return;
    campaign_over_ = true;
    if (!screens_.show_campaign_score(campaign_score(*session_, *g, ui_.text, victory, tally_))) notes_.push_back("campaign score dialog missing");
    std::printf("campaign end: %s\n", victory ? "victory" : "defeat");
}

// The holocron (IDD_DROID_LOG / DATA_CHART / TECH_TREE_* / SUMMARY_SCREEN_*): every page's data from the galaxy, then
// the page; switching pages inside keeps the data (InfoScreens).
void App::open_holocron(ui::InfoPage page) {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return;
    info_.set_droid_log(holo_.droid_log(*session_, data_, ui_.text));
    info_.set_data_chart(holo_.chart(*g, data_, ui_.text));
    info_.set_tech_tree(holo_.tech_tree(*g, data_, ui_.hud_types, local_player(), false));
    info_.set_summary(holo_.summary(*g, data_, ui_.text, ui_.hud_types, local_player()));
    if (!info_.show_info(page)) notes_.push_back("holocron page missing");
}

// GALACTIC_DIALOGS §2.2: IDD_STORY_SUMMARY_DIALOG from the mission holocron - the goal list = the chapters' titles (Active
// tab; [port] Completed lists none: chapter completion has no source yet), the recap = the newest chapter's body. Opening
// stops the holocron flash and holds the ticks until it closes (the summary's galactic pause in single player).
void App::open_story_summary() {
    if (!session_) return;
    ui::StorySummaryInfo s;
    s.completed = summary_completed_;
    s.has_completed = false;
    if (!summary_completed_ && !holocron_chapters_.empty()) {
        for (const HolocronChapter& ch : holocron_chapters_) s.goals.push_back(ch.title.empty() ? ui::utf8_to_utf32(ch.dialog) : ch.title);
        const size_t sel = summary_chapter_ >= 0 && static_cast<size_t>(summary_chapter_) < holocron_chapters_.size()
                               ? static_cast<size_t>(summary_chapter_) : holocron_chapters_.size() - 1;
        const HolocronChapter& ch = holocron_chapters_[sel];
        s.selected = static_cast<int32_t>(sel);
        s.title = ch.title;
        s.recap = ch.lines;
        s.recap_colors = ch.colors;
    }
    if (!info_.show_story_summary(s)) {
        notes_.push_back("story summary dialog missing");
        return;
    }
    holocron_flash_ = false;
    story_summary_open_ = true;
}

bool App::story_controls_locked() const {
#ifdef ALAMO_SIM_HAVE_STORY
    if (sim::StorySession* story = session_ ? session_->host().story() : nullptr)
        if (const lua::PresentationLog* log = story->presentation()) return log->state().controls_locked;
#endif
    return false;
}

// Encyclopedia pop-up for what the cursor is over (presentation only): galactic = a planet (its type), battle = the
// nearest object whose screen position is within 24 px of the cursor [port]. Delays from the panel's TooltipConfig.
void App::build_encyclopedia(ui::HudDrawList& dl, const ui::HudCanvas& canvas) {
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    uint32_t object = 0;
    objects::TypeIndex type = objects::kNone;
    const uint32_t w = targets_.width, h = targets_.height;
    if (screen_ == Screen::Galactic && gview_.attached() && !ui_.gui.hit(mx, my)) {  // HUD_GAPS §6: not under a dialog
        const ui::View v = ui::make_view(gview_.camera_params(w, h), w, h);
        const std::vector<ui::PickSphere> items = gview_.pick_items();
        if (const auto hit = ui::pick_spheres(items, ui::pixel_ray(v, mx, my)); hit && items[*hit].kind == ui::PickKind::Planet) {
            object = items[*hit].id;
            for (const render::GalacticPlanetSnap& p : gview_.frame().planets)
                if (static_cast<uint32_t>(p.id) == object) type = p.type;
        }
    } else if (screen_ == Screen::Battle && bview_.active()) {
        // HUD_GAPS §2.2: the hover pick of build_tactical_hud (bounding-sphere ray pick, reticle owner kept).
        for (const ui::HudUnit& u : hud_units_)
            if (u.id == hover_ship_ && hover_ship_ != 0 && u.type < data_.types.size()) {
                object = u.id;
                type = u.type;
            }
    }
    if (routed_.hover_component >= 0) object = 0;  // over the HUD: no world pop-up
    if (type == objects::kNone) object = 0;
    int32_t component = -1;
    // FIXPASS_GALACTIC §2.2 hover: a good hero slot shows its hero's pop-up.
    if (hero_panel_.open() && hero_panel_.hover_component() >= 0 && hero_panel_.hover_hero() != 0)
        for (const auto& [id, t] : hero_types_)
            if (id == hero_panel_.hover_hero()) {
                component = hero_panel_.hover_component();
                object = id;
                type = t;
            }
    ency_.update(component, object, now_s_);  // every frame: the countdown / fade run on the panel
    // Remember the hovered object's type so the pop-up of shown_object() still draws while it fades out; keep only the
    // shown and the hovered subjects.
    if (object != 0) {
        bool known = false;
        for (auto& [id, t] : ency_types_)
            if (id == object) {
                t = type;
                known = true;
            }
        if (!known) ency_types_.emplace_back(object, type);
    }
    const uint32_t shown = ency_.shown_object();
    std::erase_if(ency_types_, [&](const auto& p) { return p.first != shown && p.first != object; });
    if (shown == 0 || ency_.stage() == ui::EncyclopediaPanel::Stage::Hidden) return;
    objects::TypeIndex shown_type = objects::kNone;
    for (const auto& [id, t] : ency_types_)
        if (id == shown) shown_type = t;
    if (shown_type == objects::kNone) return;
    ui::EncyclopediaEntry e = encyclopedia_entry(data_, ui_.text, ui_.hud_types, shown_type);
    // GRC 2 / REMAINING_GAPS §6.3: a planet's income row only with the local player's CreditIncome visibility.
    if (screen_ == Screen::Galactic)
        if (const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr)
            if (const PlanetIndex pi = planet_index(*g, shown); pi != kInvalidPlanet &&
                !g->can_see_planet_info(local_player(), pi, galactic::GalacticSim::PlanetInfo::CreditIncome))
                std::erase_if(e.rows, [](const ui::EncyclopediaRow& r) { return r.left == U"Income"; });
    // HUD_GAPS §2.2: anchored at the hint bone of the HUD on screen (not the cursor).
    const ui::HudLayout& layout = screen_ == Screen::Battle ? ui_.tactical_hud.layout() : ui_.galactic_hud.layout();
    if (ency_.build(dl, canvas, e, layout, ui_.faction_alt()).x > 0.0f) ++ency_shown_;
}

void App::update_cursor(ui::HudDrawList& dl) {
    // ui ask 1: one cursor - the OS arrow is hidden over the window while the game draws its own.
    set_os_cursor_hidden(cursor_ok_ && !hardware_mouse_ && !opt_.hidden);
    if (!cursor_ok_ || hardware_mouse_) return;
    ui::CursorContext c;
    c.movie = screen_ == Screen::Movie;
    c.busy = screen_ == Screen::SessionLoad;
    c.over_ui = routed_.hover_component >= 0 || ui_.gui.hit(input_.mouse_x(), input_.mouse_y());
    c.drag_selecting = input_.button(ui::Button::Left).dragging && screen_ == Screen::Battle;
    c.selection = !orders_.selection().empty();
    if (screen_ == Screen::Galactic) {
        const ui::GalacticDrag& d = ui_.galactic_hud.drag();
        c.fleet_drag = d.active;
        c.drop_on_fleet = d.over_fleet;
        c.drop_on_planet = d.over_planet >= 0;
        c.drop_valid = d.valid;
        if (d.over_planet >= 0 && static_cast<size_t>(d.over_planet) < hud_planet_ids_.size() && session_ &&
            !session_->move_target_revealed(hud_planet_ids_[static_cast<size_t>(d.over_planet)]))
            c.drop_valid = false;  // GRC 1.1: an unrevealed planet is no drop target
    } else if (screen_ == Screen::Battle) {
        c.hover = c.selection ? ui::CursorContext::Hover::Ground : ui::CursorContext::Hover::None;
        // A running pick: the targeted-ability cursor (valid on a hit object / any ground point) or the landing-point one.
        if (!c.over_ui && bview_.active()) {
            const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
            const sim::TacticalBattle* tb = session_ ? session_->battle() : nullptr;
            if (targeting_.active() && tm && tb) {
                bool hit_friendly = false;
                if (targeting_.target_kind() == ui::TargetKind::Object) {
                    const ui::View v = ui::make_view(bview_.camera_params(targets_.width, targets_.height), targets_.width, targets_.height);
                    tactical::QueryContext qc;
                    const auto hit = ui::pick(tm->trees().all(), ui::pixel_ray(v, input_.mouse_x(), input_.mouse_y()), tactical::kSkipDead, qc);
                    c.target_valid = hit.has_value();  // no object under the cursor: the pick keeps waiting
                    hit_friendly = hit && !tb->combat().is_enemy(local_player(), tb->combat().owner(static_cast<tactical::combat::ObjectId>(hit->id)));
                }
                c.targeting = ui::ability_pick_cursor(targeting_.target_kind(), targeting_.ability_type(), bview_.space(), hit_friendly);
            } else if (reinforce_unit_ != 0) {
                c.targeting = ui::CursorContext::Targeting::Reinforcement;
            }
        }
        // HUD_GAPS §1.4: the repair cursor over an own / allied damaged hard point not being repaired, when the selection
        // can repair hard points.
        if (!c.over_ui && c.selection && c.targeting == ui::CursorContext::Targeting::None && !hud_hardpoints_.empty() && bview_.active()) {
            const ui::View v = ui::make_view(bview_.camera_params(targets_.width, targets_.height), targets_.width, targets_.height);
            const int32_t r = ui::reticle_at(ui_.tactical_hud.canvas(), v, reticles_, hud_hardpoints_,
                                             static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y()), now_s_);
            if (r >= 0) {
                const ui::HudHardPoint& hp = hud_hardpoints_[static_cast<size_t>(r)];
                if (!hp.enemy && hp.health < 1.0f && !hp.repairing && selection_can_repair())
                    c.targeting = ui::CursorContext::Targeting::Repair;
            }
        }
    }
    cursor_.update(ui::choose_pointer(c), now_s_);
    cursor_.build(dl, ui_.gui.canvas(), {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())}, now_s_);
}

// Panels of the current screen into `lists` (one draw list per open panel; their shells follow open()).
void App::build_panels(std::vector<ui::HudDrawList>& lists) {
    const ui::Vec2f cur{static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    auto frame_of = [&](ui::PanelFrame& f, bool open) {
        f.open = open;
        f.faction_alt = ui_.faction_alt();
        f.shells_loaded = panel_shells_ok_;
        f.time = now_s_;
        f.cursor_px = cur;
    };
    // Hero roster (battle): the local player's named heroes on the map.
    std::vector<ui::HeroEntry> heroes;
    hero_types_.clear();
    if (screen_ == Screen::Battle && bview_.active())
        for (const render::SnapshotObject& o : bview_.frame().objects)
            if (o.owner == local_player() && o.type < data_.types.size() && data_.types.get_bool(o.type, "Is_Named_Hero") && heroes.size() < 11) {
                ui::HeroEntry e;
                e.object = static_cast<uint32_t>(o.id);
                e.icon = o.type < ui_.hud_types.size() ? ui_.hud_types[o.type].icon_name : std::string();
                e.health = o.health_fraction;
                e.selected = std::binary_search(orders_.selection().begin(), orders_.selection().end(), static_cast<uint32_t>(o.id));
                // FIXPASS_GALACTIC §2.3: container = the hero's team container (AbilitySystem team membership); none ->
                // kNoObject (a double click then selects every object of the hero's type).
                e.container = commands::kNoObject;
                if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr)
                    if (const tactical::combat::ObjectId c = b->abilities().container_of(static_cast<tactical::combat::ObjectId>(o.id));
                        c != tactical::combat::kNoObject)
                        e.container = static_cast<uint32_t>(c);
                e.tooltip = type_label(data_, ui_.text, o.type);
                hero_types_.emplace_back(e.object, o.type);
                heroes.push_back(e);
            }
    // HUD_GAPS §3.2: the galactic roster (good set) every frame - the local player's named heroes on the map, then the
    // ones being built / awaiting respawn, max 11.
    if (screen_ == Screen::Galactic)
        if (const galactic::GalacticSim* gs = session_ ? session_->galaxy() : nullptr) {
            const std::vector<uint32_t> sel = orders_.selection();
            galactic_heroes_ = galactic_hero_entries(
                *gs, local_player(),
                [this](uint32_t t) { return t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string(); }, sel, 11,
                [this](uint32_t t) { return t < data_.types.size() ? data_.types.get_int(static_cast<objects::TypeIndex>(t), "Ranking_In_Category") : 0; });
            heroes = galactic_heroes_;
            for (ui::HeroEntry& e : heroes)
                if (const objects::TypeIndex t = data_.types.resolve_name(e.type_name); t != objects::kNone) {
                    e.tooltip = type_label(data_, ui_.text, t);
                    if (e.object != 0 && e.object != commands::kNoObject) hero_types_.emplace_back(e.object, t);
                }
        }
    ui::HeroPanelFrame hf;
    frame_of(hf, !heroes.empty());
    // FIXPASS_GALACTIC §2.1: the good bar is the local player's own (self / allied heroes), whatever the faction.
    hf.good = true;
    hf.tactical = screen_ == Screen::Battle;
    hf.heroes = heroes;
    hero_panel_.build(hf);
    const bool galactic = screen_ == Screen::Galactic;
    // P4 host: the open special dialog (GameSession::special_dialog, else the smoke's preview) feeds its panel from the
    // GalacticSim getters (game_info.cpp special_panel).
    const uint32_t kind = panel_kind();
    const SpecialDialog* dlg = session_ ? session_->special_dialog() : nullptr;
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    PlanetIndex pi = kInvalidPlanet;
    uint32_t planet_id = commands::kNoObject, hero_id = 0;
    if (g && kind) {
        planet_id = dlg ? dlg->planet_id : preview_planet_;
        hero_id = dlg ? dlg->hero_id : 0;
        pi = dlg ? dlg->planet : planet_index(*g, preview_planet_);
    }
    SpecialPanelData pd;
    if (g && pi != kInvalidPlanet) pd = special_panel(*g, data_, ui_.text, ui_.hud_types, local_player(), kind, pi, hero_id);
    const bool show = galactic && pi != kInvalidPlanet;
    ui::BlackMarketFrame mf;
    frame_of(mf, show && kind == galactic::kSpecialBlackMarket);
    mf.planet = planet_id;
    mf.title = pd.title;
    mf.subtitle = pd.subtitle;
    mf.items = pd.market;
    market_.build(mf);
    // OI host: the slice dialog (kind 2): SlicePanel on the same market shell (only one of the two is open).
    ui::SliceFrame lf;
    frame_of(lf, show && kind == galactic::kSpecialSlice);
    lf.planet = planet_id;
    lf.slicer = hero_id;
    lf.title = pd.title;
    lf.description = pd.subtitle;
    lf.rows = pd.slice;
    slice_.build(lf);
    ui::SabotageFrame sf;
    frame_of(sf, show && kind == galactic::kSpecialSabotage);
    sf.planet = planet_id;
    sf.title = pd.title;
    sf.subtitle = pd.subtitle;
    sf.steal_text = pd.hero;
    sf.targets = pd.sabotage;
    sabotage_.build(sf);
    ui::CorruptionFrame cf;
    frame_of(cf, show && kind == galactic::kSpecialCorruption);
    cf.planet = planet_id;
    cf.planet_title = pd.subtitle;
    cf.planet_subtitle = pd.title;
    cf.defiler_text = pd.hero;
    cf.options = pd.corruption;  // P4 host: requirement n/m rows, enabled by corruption_requirements (all four checks)
    corruption_.build(cf);
    ui::HackFrame kf;
    frame_of(kf, show && kind == galactic::kSpecialHack);
    kf.planet = planet_id;
    kf.target = pd.hack_target;
    kf.title = pd.title;
    kf.cost = pd.hack_cost;
    kf.lines = pd.hack_lines;
    kf.enabled = pd.hack_enabled;
    hack_.build(kf);
    // Panels may share a shell (market / slice): hide every panel shell, then show the open panels' active ones.
    for (ui::HudPanel* p : panels())
        for (const int32_t s : p->shells())
            if (s >= 0) panel_shells_.set_visible(static_cast<uint32_t>(s), false);
    for (ui::HudPanel* p : panels()) {
        if (const int32_t s = p->active_shell(); s >= 0) panel_shells_.set_visible(static_cast<uint32_t>(s), true);
        if (p->open()) lists.push_back(p->draw_list());
    }
}
// ------------------------------------------------------------------------------------------------ HUD frames
// g_planet_fleet faction symbol index of `player` (0 rebel, 1 empire, 2 pirate, 3 underworld, 4 hutt) by faction name
// [port]; -1 unknown.
int32_t App::galactic_faction_icon(const galactic::GalacticSim& g, int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= g.players().size()) return -1;
    const uint32_t fi = g.players()[static_cast<size_t>(player)].faction;
    if (fi >= g.rules().factions.size()) return -1;
    std::string n = g.rules().factions[fi].name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("rebel") != std::string::npos) return 0;
    if (n.find("empire") != std::string::npos) return 1;
    if (n.find("pirate") != std::string::npos) return 2;
    if (n.find("underworld") != std::string::npos) return 3;
    if (n.find("hutt") != std::string::npos) return 4;
    return -1;
}

// R2 t01: the owner faction's colour from the Factions.xml table (render::load_faction_colors), not a guess: text = true
// -> <Display_Font_Color> (planet labels / Planet Info name: GALACTIC_DIALOGS §9.2 [H] reads faction +0xb8..+0xc4;
// that the map labels use it too is [M]), else <Color> (pancake secondary colour, ring / selection tint). An unowned
// planet or a player of the Neutral faction takes the Neutral faction's entry; `fallback` when the table has none.
ui::Rgba8 App::faction_rgba(const galactic::GalacticSim& g, int32_t player, bool text, ui::Rgba8 fallback) const {
    const galactic::GalacticRules& rules = g.rules();
    uint32_t fi = rules.neutral_faction();
    if (player >= 0 && static_cast<size_t>(player) < g.players().size()) fi = g.players()[static_cast<size_t>(player)].faction;
    if (fi >= rules.factions.size()) return fallback;
    materials::float4 c;
    const std::string_view name = rules.factions[fi].name;
    if (!(text ? render::faction_text_color(faction_colors_, name, c) : render::faction_map_color(faction_colors_, name, c))) return fallback;
    auto to8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {to8(c.x), to8(c.y), to8(c.z), 255};
}

// Planet overlay icons (ui pass 2): the strongest garrison unit (AI combat power, then type index), the heroes at the
// planet (surface and orbit), and one stack per fleet (count = members, 1..5) plus one land-force stack per owner.
// GALAXY_MAP_PRESENTATION §1.4 (0x3d15b0): the representative unit of a fleet / land force - every unit competes on max
// tactical health (Tactical_Health; a hero needs >=, a non-hero >); with the hero filter (b_filters3) on, the hero with the
// lowest Ranking_In_Category wins. [port] the hero filter state is not fed to the host yet (off); transports'
// cargo is already in the fleet contents.
std::string App::representative_icon(std::span<const objects::ObjectHandle> units, bool hero_filter) const {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return {};
    const objects::ObjectStore& st = g->store();
    objects::TypeIndex best = objects::kNone, hero = objects::kNone;
    float best_hp = -1.0f;
    int32_t hero_rank = 0;
    for (const objects::ObjectHandle h : units) {
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= ui_.hud_types.size() || ui_.hud_types[t].icon_name.empty()) continue;
        const bool is_hero = is_named_hero(*g, static_cast<uint32_t>(t)) || ui_.hud_types[t].is_hero;
        const float hp = t < data_.types.size() ? data_.types.get_float(t, "Tactical_Health") : 0.0f;
        if (best == objects::kNone || (is_hero ? hp >= best_hp : hp > best_hp)) {
            best = t;
            best_hp = hp;
        }
        if (is_hero) {
            const int32_t r = t < data_.types.size() ? data_.types.get_int(t, "Ranking_In_Category") : 0;
            if (hero == objects::kNone || r < hero_rank) {
                hero = t;
                hero_rank = r;
            }
        }
    }
    const objects::TypeIndex pick = hero_filter && hero != objects::kNone ? hero : best;
    return pick == objects::kNone ? std::string() : ui_.hud_types[pick].icon_name;
}

void App::planet_icons(const galactic::GalacticSim& g, PlanetIndex row, ui::GalacticPlanetInfo& out) const {
    const objects::ObjectStore& st = g.store();
    const galactic::PlanetState& ps = g.planets()[row];
    auto icon = [this](objects::TypeIndex t) { return t < ui_.hud_types.size() ? ui_.hud_types[t].icon_name : std::string(); };
    auto is_hero = [this](objects::TypeIndex t) { return t < ui_.hud_types.size() && ui_.hud_types[t].is_hero; };
    // GRC 2 / REMAINING_GAPS §6.3 (can_see_planet_info for the local player): other players' forces show only as far as
    // the planet's visibility allows - fleet stacks with FleetPresence (one stack per owner unless NumFleets), their
    // size and heroes with FleetContents, the stack's representative icon with FleetContents or MostPowerfulShip, land
    // stacks with NumGroundCompanies, the garrison icon and land heroes with GroundCompanyContents. Own and allied
    // objects are always shown [port]. [port: Minor/MajorStealthHeroes not applied - which heroes are minor or major
    // stealth heroes is not traced (STRATEGIC_GAPS); an all-stealthed enemy fleet stays hidden as before.]
    using PI = galactic::GalacticSim::PlanetInfo;
    const int32_t me = local_player();
    auto see = [&](PI what) { return g.can_see_planet_info(me, row, what); };
    const bool fleet_presence = see(PI::FleetPresence), fleet_contents = see(PI::FleetContents);
    const bool ground_count = see(PI::NumGroundCompanies), ground_contents = see(PI::GroundCompanyContents);
    const bool num_fleets = see(PI::NumFleets), most_powerful = see(PI::MostPowerfulShip);
    auto friendly = [&](int32_t owner) { return me >= 0 && owner >= 0 && g.allied(me, owner); };
    float best = -1.0f;
    objects::TypeIndex best_t = objects::kNone;
    std::map<int32_t, uint32_t> land;  // owner -> surface units (owner order)
    for (const objects::ObjectHandle h : ps.surface) {
        if (!st.alive(h)) continue;
        const objects::TypeIndex t = st.type(h);
        if (!friendly(st.owner(h)) && !ground_contents) {
            if (ground_count && !is_hero(t)) ++land[st.owner(h)];
            continue;
        }
        if (is_hero(t)) {
            // GALAXY_MAP_PRESENTATION §1.5: the medal of the hero's faction (faction-icon order: 0 rebel, 1 empire, 3 underworld).
            const int32_t fx = st.owner(h) >= 0 ? galactic_faction_icon(g, st.owner(h)) : -1;
            out.hero_rebel = out.hero_rebel || fx == 0;
            out.hero_empire = out.hero_empire || fx == 1;
            out.hero_underworld = out.hero_underworld || fx == 3;
            continue;
        }
        ++land[st.owner(h)];
        const float power = t < g.rules().types.size() ? g.rules().types[t].ai_combat_power : 0.0f;
        if (power > best || (power == best && t < best_t)) {
            best = power;
            best_t = t;
        }
    }
    if (best_t != objects::kNone) out.garrison_icon = icon(best_t);
    // GALAXY_MAP_PRESENTATION §1.3 / §1.4: one button per orbiting fleet - unit count, the representative unit's icon,
    // stealth (every unit galactic-stealthed) / raid (ConflictManager::raid_force) badges, enemy flag; the fleet's own
    // orbit slot comes from the sim (FleetState::slot, playtest 3 t07; -1 = first free; the ui moves enemy fleets to
    // slot 0 when the orbit is not the local player's). A stealthed enemy fleet is not shown.
    out.orbit_local = me >= 0 && !g.blockaded(row, me);
    const galactic::ConflictManager* cm = session_ && session_->battles() ? &session_->battles()->conflicts() : nullptr;
    // Other players' fleets without NumFleets: one merged stack per owner (owner order) so the fleet count stays hidden.
    struct Merged { std::vector<objects::ObjectHandle> live; bool all_stealth = true; uint32_t fleet = 0; };
    std::map<int32_t, Merged> merged;
    auto push_stack = [&](int32_t owner, bool own, const std::vector<objects::ObjectHandle>& live, bool all_stealth, uint32_t fleet,
                          int32_t orbit_slot) {
        const bool contents = own || fleet_contents;
        ui::GalacticPlanetStack s;
        s.faction = std::max(0, galactic_faction_icon(g, owner));
        s.owner_color = faction_rgba(g, owner, false, ui::Rgba8{});  // §1.4 secondary colour (white when the table has none)
        s.units = contents ? static_cast<uint32_t>(live.size()) : 1u;
        s.count = std::clamp<uint32_t>(s.units, 1, 5);
        if (contents || most_powerful) s.icon = representative_icon(live);
        s.enemy = owner != me;
        s.stealth = all_stealth;
        s.raid = !s.stealth && cm && cm->raid_force(owner, live);
        s.selected = fleet != 0 && !orders_.selection().empty() && orders_.selection().front() == fleet;
        // The button's fleet: a click selects it (GalacticSelect), an own fleet's button (enemy = false) drags it.
        s.fleet = fleet;
        s.slot = orbit_slot;
        // Orbiting heroes show only in their fleet's slot, as in the original (Gabriel, playtest 3, 2026-10-29).
        out.stacks.push_back(std::move(s));
    };
    for (const galactic::FleetState& fl : g.fleets()) {
        if (!fl.alive || fl.planet != row || fl.contents.empty()) continue;
        const bool own = friendly(fl.owner);
        if (!own && !fleet_presence) continue;
        std::vector<objects::ObjectHandle> live;
        bool all_stealth = true;
        for (const objects::ObjectHandle m : fl.contents)
            if (st.alive(m)) {
                live.push_back(m);
                all_stealth = all_stealth && g.hidden(m);
            }
        if (live.empty() || (!own && all_stealth)) continue;
        const uint32_t fleet = st.alive(fl.object) ? static_cast<uint32_t>(st.id(fl.object)) : 0u;
        if (!own && !num_fleets) {
            Merged& m = merged[fl.owner];
            m.live.insert(m.live.end(), live.begin(), live.end());
            m.all_stealth = m.all_stealth && all_stealth;
            if (m.fleet == 0) m.fleet = fleet;
            continue;
        }
        push_stack(fl.owner, own, live, all_stealth, fleet, fl.slot);
    }
    for (const auto& [owner, m] : merged) push_stack(owner, false, m.live, m.all_stealth, m.fleet, -1);
    for (const auto& [owner, n] : land) {
        ui::GalacticPlanetStack s;
        s.faction = std::max(0, galactic_faction_icon(g, owner));
        s.owner_color = faction_rgba(g, owner, false, ui::Rgba8{});
        s.units = n;
        s.count = std::clamp<uint32_t>(n, 1, 5);
        s.enemy = owner != me;
        s.land = true;
        // Land forces: a click selects the planet (its land organiser) for the local player's own forces [port: the
        // original's land-forces click target is not specified]; other players' land forces are not clickable. Right
        // presses pass through to the planet (RouteHooks::hud_takes).
        if (owner == me && st.alive(ps.object)) s.fleet = static_cast<uint32_t>(st.id(ps.object));
        if (friendly(owner) || ground_contents) {
            std::vector<objects::ObjectHandle> units;
            for (const objects::ObjectHandle h : ps.surface)
                if (st.alive(h) && st.owner(h) == owner) units.push_back(h);
            s.icon = representative_icon(units);
        }
        out.stacks.push_back(std::move(s));
    }
}

// GALACTIC_DIALOGS §9.2: IDD_PLANET_INFO from the planet TYPE (static XML text); no live sim state and no
// REMAINING_GAPS §6.3 gating (the original shows the same dialog for enemy, fogged or stealthed planets). Only the name
// colour reveals the owner: the owner faction's Factions.xml <Display_Font_Color>, white when unowned or neutral.
// IDC_PLANET_ICON = the planet type's portrait (render::PlanetPortraits, "planet_portrait:<type>"); planet_overlay "" =
// load_overlay.tga.
ui::PlanetInfoData App::planet_info_data(const galactic::GalacticSim& g, PlanetIndex row) const {
    ui::PlanetInfoData d;
    if (row >= g.planets().size()) return d;
    const galactic::PlanetState& ps = g.planets()[row];
    planet_type_info(d, ps.type < data_.types.size() ? static_cast<objects::TypeIndex>(ps.type) : objects::kNone);
    if (d.name.empty() && row < g.map().planets.name.size()) d.name = ui::utf8_to_utf32(g.map().planets.name[row]);
    const int32_t own = ps.owner;
    if (own >= 0 && static_cast<size_t>(own) < g.players().size()) {
        const uint32_t fx = g.players()[static_cast<size_t>(own)].faction;
        if (fx < g.rules().factions.size() && fx != g.rules().neutral_faction()) {
            // R2 t01: <Display_Font_Color> (faction +0xb8..+0xc4, GALACTIC_DIALOGS §9.2 [H]), was <Color>.
            d.name_color = faction_rgba(g, own, true, d.name_color);
        }
    }
    return d;
}

// The static text and icons of a planet type (GALACTIC_DIALOGS §9.2): IDD_PLANET_INFO and the battle load screens.
void App::planet_type_info(ui::PlanetInfoData& d, objects::TypeIndex t) const {
    const bool typed = t != objects::kNone && t < data_.types.size();
    auto field = [&](std::string_view tag) {
        std::string_view v = typed ? data_.types.get_string(t, tag) : std::string_view{};
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
        return v;
    };
    auto text = [&](std::string_view tag) {
        const std::string_view id = field(tag);
        if (id.empty()) return std::u32string();
        const std::u16string* txt = ui_.text.find(id);
        return txt ? ui::utf16_to_utf32(*txt) : ui::utf8_to_utf32(id);
    };
    d.name = text("Text_ID");
    if (std::u32string v = text("Describe_Tactical"); !v.empty()) d.tactical.push_back(std::move(v));
    if (std::u32string v = text("Describe_Advantage"); !v.empty()) d.advantages.push_back(std::move(v));
    if (std::u32string v = text("Describe_History"); !v.empty()) d.history.push_back(std::move(v));
    if (typed && t < data_.rules.types.size()) d.planet_icon = render::planet_portrait_name(data_.rules.types[t].name);
    d.natives = text("Describe_Population");
    d.terrain = text("Describe_Terrain");
    d.weather = text("Describe_Weather");
    d.weather_icon = ui::planet_info_weather_icon(field("Encyclopedia_Weather_Icon"));
    d.native_icon = std::string(field("Native_Icon_Name"));
    // IDC_MAP_ICON: Land_Tactical_Map .ted -> .dds, switched off when the texture does not resolve (the Art/Textures
    // folder is among the lookup candidates, assets::texture_candidates).
    if (const std::string map = ui::planet_info_map_icon(field("Land_Tactical_Map")); !map.empty())
        for (const std::string& c : assets::texture_candidates(map))
            if (data_.files->exists(c)) {
                d.map_icon = map;
                break;
            }
}

// QA U1: the place a battle is fought at, localized; a skirmish map's planet type (_MP_SPACE_KUAT -> Kuat). "" when
// unknown: never the map file name.
std::u32string App::battle_place_name(PlanetIndex planet) const {
    if (!session_) return {};
    if (session_->spec().kind == SessionKind::Skirmish) {
        const uint32_t t = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
        return t != galactic::kNoIndex && t < data_.types.size() ? type_label(data_, ui_.text, static_cast<objects::TypeIndex>(t)) : std::u32string();
    }
    const galactic::GalacticSim* g = session_->galaxy();
    return g && planet < g->planets().size() ? planet_label(data_, ui_.text, *g, planet) : std::u32string();
}

// QA t16 (UI.md §5.1 step 1, §5.2): fills the load dialog from the planet type's text, as IDD_PLANET_INFO (GALACTIC_DIALOGS
// §9.2) [inferred: the same fields; the fill function of the load dialog is not traced field by field]. QA t11: the sim
// is held until it has been presented (end_load_intro_frame); no load moved off this thread.
bool App::open_battle_load(PlanetIndex planet, bool space) {
    if (ui_.load.is_open()) return true;
    if (!session_) return false;
    const bool skirmish = session_->spec().kind == SessionKind::Skirmish;
    ui::PlanetInfoData d;
    if (skirmish) {
        const uint32_t t = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
        if (t != galactic::kNoIndex) planet_type_info(d, static_cast<objects::TypeIndex>(t));
    } else if (const galactic::GalacticSim* g = session_->galaxy()) {
        d = planet_info_data(*g, planet);
        // The displayed planet type's portrait (render::PlanetPortraits) as the planet icon.
        if (planet < g->planets().size())
            if (const uint32_t pt = display_planet_type(*g, planet); pt < g->rules().types.size())
                d.planet_icon = render::planet_portrait_name(g->rules().types[pt].name);
    }
    // UI.md §5.1 step 1: the land or space picture by battle kind; IDD_PLANET_INFO's land map on a space battle would show
    // the wrong place [inferred: Space_Tactical_Map, hidden when its .dds does not exist].
    if (space) {
        d.map_icon.clear();
        objects::TypeIndex t = objects::kNone;
        if (skirmish) {
            const uint32_t st = data_.rules.find_type(game::skirmish_map_planet_type(data_.rules, session_->spec().skirmish_map));
            if (st != galactic::kNoIndex) t = static_cast<objects::TypeIndex>(st);
        } else if (const galactic::GalacticSim* g = session_->galaxy(); g && planet < g->planets().size()) {
            t = static_cast<objects::TypeIndex>(g->planets()[planet].type);
        }
        if (t != objects::kNone && t < data_.types.size())
            if (const std::string map = ui::planet_info_map_icon(data_.types.get_string(t, "Space_Tactical_Map")); !map.empty())
                for (const std::string& c : assets::texture_candidates(map))
                    if (data_.files->exists(c)) {
                        d.map_icon = map;
                        break;
                    }
    }
    ui::LoadScreenInfo info;
    info.planet_name = battle_place_name(planet);
    info.tactical = std::move(d.tactical);
    info.advantages = std::move(d.advantages);
    info.history = std::move(d.history);
    info.terrain = std::move(d.terrain);
    info.weather = std::move(d.weather);
    info.natives = std::move(d.natives);
    info.planet_icon = std::move(d.planet_icon);
    info.weather_icon = std::move(d.weather_icon);
    info.map_icon = std::move(d.map_icon);
    info.native_icon = std::move(d.native_icon);
    info.begin_button = !session_->spec().attract;  // OI host: the attract demo starts as soon as it is loaded
    const ui::LoadScreenKind kind = skirmish ? ui::LoadScreenKind::Skirmish : space ? ui::LoadScreenKind::SpaceBattle : ui::LoadScreenKind::LandBattle;
    screens_.close_planet_info();  // GALACTIC_DIALOGS §9.4: IDD_PLANET_INFO never survives into a battle
    ui_.load.set_progress_source([this] { return bview_.active() ? bview_.load_progress() : 0.0f; });
    if (!ui_.load.open(ui_.gui, kind, info)) return false;
    audio_.music(MusicScene::LoadScreen, local_faction());
    load_intro_frames_ = kLoadIntroMaxFrames;
    load_intro_shown_ = 0;
    load_open_tick_ = session_->host().frame();
    return true;
}

// After each presented frame: the hold ends once the dialog has been drawn complete (its textures and the planet portrait
// are uploaded over the first frames) or after kLoadIntroMaxFrames. Also: a Fight answer that never became a battle (the
// conflict was dropped or auto-resolved instead) closes the load screen again.
void App::end_load_intro_frame() {
    if (load_intro_frames_ > 0) {
        ++load_intro_shown_;
        bool pending = load_intro_shown_ < 2;
        for (const ui::GuiImagePages::Entry& e : ui_.pages.entries()) pending = pending || (e.width == 0 && !e.failed);
        load_intro_frames_ = pending ? load_intro_frames_ - 1 : 0;
        if (load_intro_frames_ == 0) std::printf("load screen: %u frames presented before the blocking load\n", load_intro_shown_);
        return;
    }
    if (screen_ == Screen::Galactic && ui_.load.is_open() && session_ && !session_->in_battle() &&
        session_->host().frame() > load_open_tick_ + kLoadIntroMaxTicks) {
        ui_.load.close();
        audio_.music(MusicScene::Galactic, local_faction());
    }
}

void App::build_galactic_hud(ui::GalacticHudFrame& f, std::vector<ui::GalacticPlanetInfo>& planets) {
    const uint32_t w = targets_.width, h = targets_.height;
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    f.view = ui::make_view(gview_.camera_params(w, h), w, h);
    planets.clear();
    for (const render::GalacticPlanetSnap& p : gview_.frame().planets) {
        if (!(p.flags & render::kGalPlanet) || (p.flags & render::kGalHidden) || p.id == objects::kNoObjectId) continue;
        ui::GalacticPlanetInfo i;
        i.planet = static_cast<uint32_t>(p.id);
        i.position = {p.position.x, p.position.y, p.position.z};
        i.radius = gview_.planet_radius(p.type);
        std::string row = g && p.row < g->map().planets.name.size() ? g->map().planets.name[p.row] : std::string();
        // Display name: the planet type's Text_ID in MasterTextFile, else the row name.
        const std::u16string* txt = nullptr;
        if (p.type != objects::kNone && p.type < data_.types.size())
            if (const std::string_view id = data_.types.get_string(p.type, "Text_ID"); !id.empty()) txt = ui_.text.find(id);
        i.name = txt ? ui::utf16_to_utf32(*txt) : ui::utf8_to_utf32(row);
        auto to8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        i.owner_color = {to8(p.team_color.x), to8(p.team_color.y), to8(p.team_color.z), 255};
        // R2 t01: the name in the owner faction's Factions.xml <Display_Font_Color> (Neutral faction's for an unowned planet);
        // the palette colour stays the fallback when the table has no such faction. [M: that the map labels read
        // Display_Font_Color is inferred from GALACTIC_DIALOGS §9.2; these values replace the ones read off a screenshot.]
        if (g && p.row < g->planets().size()) {
            const int32_t own = g->planets()[p.row].owner;
            const int32_t fx = own >= 0 ? galactic_faction_icon(*g, own) : -1;
            i.owner_color = faction_rgba(*g, own, true, i.owner_color);
            // GALACTIC_DIALOGS §5.3: the allegiance emblem index (0 rebel, 1 empire, 2 underworld, 3 pirate, 4 hutt; the
            // faction-icon order above swaps pirate / underworld); neutral or unowned -> none.
            static constexpr int32_t kAllegiance[] = {0, 1, 3, 2, 4};
            if (fx >= 0 && g->players()[static_cast<size_t>(own)].faction != g->rules().neutral_faction())
                i.allegiance = kAllegiance[fx];
        }
        // Fog (render apply_galactic_fog: kGalFogged), garrison / hero icons and the fleet / land-force stacks.
        i.fogged = (p.flags & render::kGalFogged) != 0;
        if (g && p.row < g->planets().size()) planet_icons(*g, p.row, i);
        i.political_control = p.control;
        i.space_level = p.starbase_level > 0 ? p.starbase_level : -1;
        i.ground_level = p.ground_level > 0 ? p.ground_level : -1;
        i.conflict = (p.flags & render::kGalInBattle) != 0;
        if (g && p.row < g->planets().size()) {  // OI host: REMAINING_GAPS §6.3 display gates for the local player
            i.starbase_info = g->can_see_starbase_info(local_player(), p.row);
            i.groundbase_info = g->can_see_groundbase_info(local_player(), p.row);
            // GRC 2: the ui gets only what the local player can see.
            if (!i.starbase_info) i.space_level = -1;
            if (!i.groundbase_info) i.ground_level = -1;
            if (!g->can_see_planet_info(local_player(), p.row, galactic::GalacticSim::PlanetInfo::PoliticalControl))
                i.political_control = -1.0f;
            // Text_Planetary_Income (GALACTIC_DIALOGS §5.3), behind the same gate as the planet tooltips. GalacticSim::planet_income
            // refreshes the sim's lists, so the host only reads: the last fiscal cycle's entries, else Planet_Credit_Value [port].
            if (!i.fogged && g->can_see_planet_info(local_player(), p.row, galactic::GalacticSim::PlanetInfo::CreditIncome)) {
                const galactic::PlanetState& ps = g->planets()[p.row];
                float sum = ps.income.empty() && ps.type < g->rules().types.size() ? static_cast<float>(g->rules().types[ps.type].credit_value) : 0.0f;
                for (const galactic::IncomeEntry& e : ps.income) sum += e.value;
                float siphoned = 0.0f;
                const float net = g->frame() <= ps.halt_until ? 0.0f
                                  : galactic::econ::income_finish(sum, ps.min_income_set, ps.min_income, ps.max_income_set, ps.max_income, false, 0.0f, &siphoned);
                i.income = static_cast<int64_t>(std::lround(net));
                // g_planet_value: TEXT_TOOLTIP_CREDIT_FILTER "credit value of planets" = the planet base value (GALACTIC_ECONOMY §1.4)
                // [M: which number the text prints is read from the tooltip, no capture with the filter on].
                if (ps.type < g->rules().types.size()) i.credits = ps.destroyed ? g->rules().types[ps.type].destroyed_credit_value : ps.base_value;
            }
        }
        planets.push_back(std::move(i));
    }
    f.planets = planets;
    hud_planet_ids_.clear();
    for (const ui::GalacticPlanetInfo& i : planets) hud_planet_ids_.push_back(i.planet);
    if (g) {
        f.credits = static_cast<int64_t>(g->credits(local_player()));
        f.day = g->galactic_day();
        f.tech = g->players()[static_cast<size_t>(local_player())].tech_level;
        // ui ask 7: population used / capacity and the income per fiscal cycle (the last cycle's gain) [port: per day].
        f.pop = g->population_used(local_player());
        f.pop_cap = g->population_capacity(local_player());
        f.income = static_cast<int64_t>(std::lround(g->last_income(local_player())));
        // HUD_GAPS §4: the day pips = the fiscal-cycle timer (mode frame - cycle start) / round(30 x Fiscal_Cycle_Time).
        f.cycle_fraction = galactic_cycle_fraction(*g);
        f.day_pip_flash = true;
    }
    f.faction_alt = ui_.faction_alt();
    f.shells_loaded = ui_.galactic_shells_ok;
    // Playtest 2 (base 24 / 26.jpg): the fleet organiser (Fleet 1..3 boxes) belongs to the zoomed planet view only - never
    // over the galaxy view, even with a fleet selected (ui ask 9's fleet-selection trigger opened it there).
    f.organize_open = gview_.camera().zoomed() && selected_planet_ != 0;
    // Playtest 3 (base 40.jpg): a selected fleet / fleet unit shows every planet's fleet-slot ovals.
    f.show_fleet_slots = !orders_.selection().empty();
    if (g) f.empty_slot_color = faction_rgba(*g, local_player(), false, ui::Rgba8{});
    f.paused = session_ && session_->pause_requested();
    for (ui::GalacticPlanetInfo& p : planets) p.selected = p.planet == selected_planet_;
    for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
        if (s.model_name.find("organize") != std::string::npos)
            ui_.galactic_shells.set_visible(s.component, f.organize_open && !planet_map_open_);
    // HUD_GAPS §4.2: the advanced planet map = the zoomed planet view [port]; map_shell is shown with it and its bones
    // (day_clock) re-applied to the layout.
    // GALACTIC_DIALOGS §4: the advanced planet map is the FoC planet map entered by the organiser's advanced_planetary
    // ("Base Layout") button - not the zoomed planet view. [port] it closes when the close-up or the selection ends; the
    // map screen itself is not built yet (only map_shell is shown).
    if (planet_map_open_ && (!gview_.camera().zoomed() || selected_planet_ == 0)) planet_map_open_ = false;
    f.advanced_map_open = planet_map_open_;
    if (f.advanced_map_open != map_shell_open_) {
        map_shell_open_ = f.advanced_map_open;
        for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
            if (static_cast<size_t>(s.component) < ui_.components.size() && ui_.components[s.component].name == "map_shell")
                ui_.galactic_shells.set_visible(s.component, map_shell_open_);
        ui_.galactic_shells.apply(ui_.galactic_hud.layout());
    }
    // GALACTIC_DIALOGS §1 banner, §2.1 holocron flash, screen text ([port] each line 6 s, the last second fading).
    if (choice_open_) {
        f.battle.active = true;
        f.battle.single = prompt_.story_pending;
        f.battle.space = !prompt_.land;
        f.battle.planet = prompt_.planet_name;
        f.battle.left = {prompt_.left.icon, prompt_.left.units, static_cast<int32_t>(prompt_.left.stacks)};
        f.battle.right = {prompt_.right.icon, prompt_.right.units, static_cast<int32_t>(prompt_.right.stacks)};
    }
    f.holocron_flash = holocron_flash_;
    screen_text_.clear();
    // t47 [port]: GALACTIC_DIALOGS §3.1 posts the red result line and opens the battle-end screen in one step; the screen fills
    // the view and the line would sit on its header, so the line waits (clock held) and shows for its 6 s once the screen closes.
    if (battle_end_open_)
        for (ScreenLine& l : screen_lines_) l.at = now_s_;
    std::erase_if(screen_lines_, [this](const ScreenLine& l) { return now_s_ - l.at > 6.0; });
    for (const ScreenLine& l : screen_lines_) {
        if (battle_end_open_) break;
        const double left = 6.0 - (now_s_ - l.at);
        screen_text_.push_back({l.text, l.color, static_cast<uint8_t>(std::clamp(left, 0.0, 1.0) * 255.0)});
    }
    f.screen_text = screen_text_;
    f.time = now_s_;
    f.cursor_px = {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    // HUD_GAPS §6: no galactic hover (planet rings, tooltips) under an open GUI dialog (the story dialog).
    if (ui_.gui.hit(input_.mouse_x(), input_.mouse_y())) f.cursor_px = {-1.0e6f, -1.0e6f};
    if (hud_hit_debug_) {
        const bool fixed = hud_hit_px_ >= 0;
        const int32_t px = fixed ? hud_hit_px_ : input_.mouse_x(), py = fixed ? hud_hit_py_ : input_.mouse_y();
        const int32_t before = hud_hit_last_;
        hud_hit_dump("galactic", ui_.galactic_hud.hit_test(px, py), px, py);
        if (fixed && before != hud_hit_last_) {
            std::printf("hud hit: galactic shells visible:");
            for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
                if (s.visible && static_cast<size_t>(s.component) < ui_.components.size())
                    std::printf(" %s", ui_.components[s.component].name.c_str());
            std::printf(" (planet %u, selection %zu)\n", selected_planet_, orders_.selection().size());
        }
    }
    const GameSession* s = session_.get();
    const std::vector<ui::HudTypeInfo>* types = &ui_.hud_types;
    f.providers.fleets = [s, types](uint32_t planet, std::vector<ui::GalacticFleet>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs) return;
        const objects::ObjectStore& st = gs->store();
        for (const galactic::FleetState& fl : gs->fleets()) {
            if (!fl.alive || fl.owner != s->local_player() || fl.planet == kInvalidPlanet) continue;
            const galactic::PlanetState& p = gs->planets()[fl.planet];
            if (!st.exists(p.object) || static_cast<uint32_t>(st.id(p.object)) != planet || out.size() >= 3) continue;
            ui::GalacticFleet gf;
            gf.fleet = static_cast<uint32_t>(st.id(fl.object));
            for (const objects::ObjectHandle m : fl.contents) {
                if (!st.alive(m)) continue;
                const objects::TypeIndex t = st.type(m);
                gf.units.push_back({static_cast<uint32_t>(st.id(m)), t < types->size() ? (*types)[t].icon_name : std::string(), 1, false});
            }
            out.push_back(std::move(gf));
        }
    };
    // OI host: build bar = GalacticSim::buildable (type-index order) at the planet for the local player; cost =
    // Build_Cost_Credits [port: no cost modifiers], enabled = affordable [port], structure = ground / orbital structure or
    // starbase (b_Create_NN); production_disabled = the player's production-disabled list (REMAINING_GAPS §1.5).
    f.providers.build_options = [s, types](uint32_t planet, std::vector<ui::GalacticBuildOption>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs) return;
        const PlanetIndex pi = planet_index(*gs, planet);
        if (pi == kInvalidPlanet) return;
        const int32_t me = s->local_player();
        const float credits = gs->credits(me);
        for (const uint32_t t : gs->buildable(pi, me)) {
            if (t >= gs->rules().types.size() || t > 0xFFFFu) continue;
            const galactic::TypeInfo& ti = gs->rules().types[t];
            ui::GalacticBuildOption o;
            o.type_index = static_cast<uint16_t>(t);
            o.icon = t < types->size() ? (*types)[t].icon_name : std::string();
            o.cost = ti.build_cost;
            o.enabled = static_cast<float>(ti.build_cost) <= credits;
            o.structure = galactic::is_ground_structure_type(ti) || galactic::is_orbital_structure_type(ti) || galactic::is_star_base_type(ti);
            o.production_disabled = gs->production_disabled(me, t);
            // GALACTIC_DIALOGS §5.1: the space tab = space-queue units, starbases and orbital structures.
            o.space = gs->queue_kind(t) == galactic::kSpaceQueue || galactic::is_star_base_type(ti) || galactic::is_orbital_structure_type(ti);
            out.push_back(std::move(o));
        }
    };
    // GALACTIC_DIALOGS §5.2: the selected planet's build queues (the local player's entries, head first): space queue on
    // queue00..04, ground queue on queue05..09 (HudQueueEntry::land); the head's timer gives the dial. A click cancels
    // through Production {cancel, queue, index} at the planet.
    f.providers.build_queue = [s, types, sel = selected_planet_](std::vector<ui::HudQueueEntry>& out) {
        const galactic::GalacticSim* gs = s ? s->galaxy() : nullptr;
        if (!gs || sel == 0) return;
        const PlanetIndex pi = planet_index(*gs, sel);
        if (pi == kInvalidPlanet) return;
        const int32_t me = s->local_player();
        const int32_t now = gs->frame();
        for (const uint32_t q : {static_cast<uint32_t>(galactic::kSpaceQueue), static_cast<uint32_t>(galactic::kGroundQueue)}) {
            const std::span<const galactic::BuildEntry> queue = gs->queue_of(pi, q);
            for (size_t i = 0; i < queue.size() && i <= 0xFFu; ++i) {
                const galactic::BuildEntry& b = queue[i];
                if (b.player != me || b.type > 0xFFFFu) continue;
                ui::HudQueueEntry e;
                e.type_index = static_cast<uint16_t>(b.type);
                e.icon = b.type < types->size() ? (*types)[b.type].icon_name : std::string();
                e.progress = (i == 0 && b.duration > 0)
                                 ? std::clamp(static_cast<float>(now - b.start) / static_cast<float>(b.duration), 0.0f, 1.0f)
                                 : 0.0f;
                e.location = sel;
                e.queue = static_cast<uint8_t>(q);
                e.index = static_cast<uint8_t>(i);
                e.land = q == galactic::kGroundQueue;
                out.push_back(std::move(e));
            }
        }
    };
    // OI host: g_special_ability rows = the local player's Ground_Activated special-ability units at the planet.
    f.providers.special_abilities = [this](uint32_t planet, std::vector<ui::GalacticSpecialAbility>& out) {
        const galactic::GalacticSim* gs = session_ ? session_->galaxy() : nullptr;
        if (!gs) return;
        const objects::ObjectStore& st = gs->store();
        for (const objects::ObjectHandle h : special_units(*gs, planet_index(*gs, planet))) {
            ui::GalacticSpecialAbility r;
            r.hero = static_cast<uint32_t>(st.id(h));
            const objects::TypeIndex t = st.type(h);
            r.text = type_label(data_, ui_.text, t);
            if (t < ui_.hud_types.size()) r.icon = ui_.hud_types[t].icon_name;
            out.push_back(std::move(r));
        }
    };
}

void App::build_tactical_hud() {
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    const uint32_t w = targets_.width, h = targets_.height;
    ui::TacticalHudFrame hf;
    hf.world.view = ui::make_view(bview_.camera_params(w, h), w, h);
    hf.world.land = !bview_.space();
    hf.world.local_player = local_player();
    if (tm && tm->bounds().max.x - tm->bounds().min.x < 1.0e5f) {
        hf.map_min_x = tm->bounds().min.x;
        hf.map_min_y = tm->bounds().min.y;
        hf.map_max_x = tm->bounds().max.x;
        hf.map_max_y = tm->bounds().max.y;
    }
    if (const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr) {
        hf.credits = static_cast<int64_t>(g->credits(local_player()));
        // ui ask 7: the battle's planet, tech level and population (used / space unit cap).
        hf.tech = g->players()[static_cast<size_t>(local_player())].tech_level;
        if (const sim::TacticalBattle* b = session_->battle()) {
            if (b->request().info.planet < g->planets().size()) {
                const uint32_t pt = display_planet_type(*g, b->request().info.planet);
                hf.planet_name = pt != g->planets()[b->request().info.planet].type && pt < data_.types.size()
                                     ? type_label(data_, ui_.text, static_cast<objects::TypeIndex>(pt))
                                     : planet_label(data_, ui_.text, *g, b->request().info.planet);
            }
            hf.pop = static_cast<int32_t>(std::lround(b->population_used(local_player())));
            const float cap = b->population_cap(local_player());
            hf.pop_cap = cap >= 0.0f ? static_cast<int32_t>(std::lround(cap)) : -1;
        }
    }
    // ui pass 2: the mission objectives box - the story's objectives, else the skirmish win condition [port text].
    hud_objectives_.clear();
#if defined(ALAMO_SIM_HAVE_STORY) && defined(ALAMO_UI_HAVE_LUA)
    if (const sim::StorySession* story = session_ ? session_->host().story() : nullptr)
        if (const lua::StoryModel* m = story->tactical_model()) hud_objectives_ = ui::hud_objectives(*m);
#endif
    if (hud_objectives_.empty() && session_ && session_->spec().kind == SessionKind::Skirmish) {
        ui::HudObjective o;
        o.text = bview_.space() ? U"1. Destroy all enemy space forces to win." : U"1. Destroy all enemy land forces to win.";
        hud_objectives_.push_back(std::move(o));
    }
    hf.objectives = hud_objectives_;
    hf.faction_alt = ui_.faction_alt();
    hf.shells_loaded = ui_.tactical_shells_ok;
    hf.time = now_s_;
    hf.cursor_px = {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    // Drag-select box: the world-owned left drag that OrderTranslator box-selects on release (same rect and threshold).
    hf.drag_select = input_.button(ui::Button::Left).dragging;
    hf.drag_rect = input_.button(ui::Button::Left).drag;
    std::vector<ui::HudUnit> units;
    const std::vector<uint32_t> sel = orders_.selection();
    // ui asks 3 / 4: the bracket radius from the model's bounding sphere x scale; one unit icon per squadron / unit.
    ui::hud_units_from_frame(bview_.frame(), sel, {}, 0, [this](objects::TypeIndex t) { return bview_.type_radius(t); }, units);
    tactical_hover(hf, units);
    if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr) {
        std::vector<ui::UnitIconLink> icons;  // QA U7: one marker per squadron, at its centre
        for (const sim::TacticalBattle::IconLink& l : b->icon_links())
            icons.push_back({static_cast<uint32_t>(l.unit), static_cast<uint32_t>(l.squad)});
        ui::assign_unit_icons(units, icons);
    }
    // Pause look, radar picture (fog of war), queue markers of the selected units (sim queued_orders, const reads).
    hf.paused = session_ && session_->pause_requested();
    update_radar();
    if (!radar_.empty()) hf.radar_image = &radar_;
    hf.radar_events = &radar_events_;
    hf.radar_settings = &radar_settings_;
    hf.radar_event_models = ui_.radar_models_ok;  // the event .alo models go through ui_.radar_drawer (Models2 step)
    radar_events_.update(dt_);
    queues_.clear();
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;  // ascending id
    auto obj_of = [&objs](objects::ObjectId id) -> const render::SnapshotObject* {
        const auto it = std::lower_bound(objs.begin(), objs.end(), id, [](const render::SnapshotObject& x, objects::ObjectId v) { return x.id < v; });
        return it != objs.end() && it->id == id ? &*it : nullptr;
    };
    if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr)
        for (const uint32_t id : sel) {
            const auto q = b->queued_orders(static_cast<objects::ObjectId>(id));
            const render::SnapshotObject* o = obj_of(static_cast<objects::ObjectId>(id));
            if (q.empty() || !o) continue;
            ui::HudOrderQueue hq;
            hq.unit = id;
            hq.position = {o->position.x, o->position.y, o->position.z};
            using QK = sim::TacticalBattle::QueuedOrder::Kind;
            for (const sim::TacticalBattle::QueuedOrder& e : q) {
                ui::HudQueuedStep s;
                s.kind = e.kind == QK::Attack ? ui::HudQueuedStep::Kind::Attack
                         : e.kind == QK::Ability ? ui::HudQueuedStep::Kind::Ability
                         : e.kind == QK::Move    ? ui::HudQueuedStep::Kind::Move
                                                 : ui::HudQueuedStep::Kind::Other;
                if (e.kind == QK::Move) {
                    s.point = {e.x, e.y, o->position.z};
                } else if (const render::SnapshotObject* tg = obj_of(e.target)) {
                    s.point = {tg->position.x, tg->position.y, tg->position.z};
                } else {
                    s.has_point = false;
                }
                hq.steps.push_back(s);
            }
            queues_.push_back(std::move(hq));
        }
    hf.order_queues = queues_;
    // Movement path lines (HudWaypointLine dots): every selected local unit with queued move points - its position, its
    // current movement goal, then those points (sim const reads between ticks). [port] goal height = the unit's.
    // [port] A plain move draws no line (playtest video R1: with a whole fleet selected, one 3 px dotted line per fighter
    // covered the battle and read as laser fire); the original's waypoint line belongs to the Alt-queued path (UI.md 1.5).
    waypoints_.clear();
    if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr)
        for (const uint32_t id : sel) {
            const auto oid = static_cast<objects::ObjectId>(id);
            const render::SnapshotObject* o = obj_of(oid);
            if (!o || o->owner != local_player() || !b->movement().has_unit(oid)) continue;
            const tactical::movement::UnitView v = b->movement().view(oid);
            if (!v.has_goal || v.arrived) continue;
            ui::HudWaypointLine l;
            l.points.push_back({o->position.x, o->position.y, o->position.z});
            l.points.push_back({v.goal.x, v.goal.y, o->position.z});
            for (const sim::TacticalBattle::QueuedOrder& e : b->queued_orders(oid)) {
                if (e.kind != sim::TacticalBattle::QueuedOrder::Kind::Move) continue;
                const ui::Vec3 p{e.x, e.y, o->position.z};
                if (p.x != l.points.back().x || p.y != l.points.back().y) l.points.push_back(p);
            }
            if (l.points.size() > 2) waypoints_.push_back(std::move(l));
        }
    hf.waypoints = waypoints_;
    // Reinforcement pad: the local player's undeployed reserves (TacticalBattle::reserves, const read between ticks).
    const GameSession* sess = session_.get();
    const std::vector<ui::HudTypeInfo>* htypes = &ui_.hud_types;
    hf.providers.reinforcements = [sess, htypes](std::vector<ui::HudReinforcement>& out) {
        const sim::TacticalBattle* b = sess ? sess->battle() : nullptr;
        if (!b) return;
        for (const sim::TacticalBattle::Reserve& r : b->reserves(sess->local_player())) {
            ui::HudReinforcement e;
            e.unit = static_cast<uint32_t>(r.galactic_id);
            e.icon = r.type < htypes->size() ? (*htypes)[r.type].icon_name : std::string();
            e.count = r.units;
            out.push_back(std::move(e));
        }
    };
    // Tactical buy bar: the first selected own builder with production options (skirmish space stations), its options
    // with price / queued count / block reason, anchored at the builder's projected pixel.
    build_opts_.clear();
    if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr; b && tm && b->production_enabled()) {
        const objects::ObjectStore& st = tm->primary();
        for (const uint32_t id : orders_.selection()) {
            if (id > 0x7FFFFFFFu) continue;
            const auto opts = b->station_build_options(st, local_player(), static_cast<objects::ObjectId>(id));
            if (opts.empty()) continue;
            hf.builder = id;
            for (const sim::TacticalBattle::BuildOptionInfo& o : opts) {
                ui::HudBuildOption e;
                e.type_index = static_cast<uint16_t>(o.type);
                e.icon = static_cast<size_t>(o.type) < ui_.hud_types.size() ? ui_.hud_types[static_cast<size_t>(o.type)].icon_name : std::string();
                e.cost = o.cost;
                e.queued = o.queued;
                e.block = static_cast<ui::HudBuildOption::Block>(o.block);
                build_opts_.push_back(std::move(e));
            }
            const Vec3 p = st.position(st.find(static_cast<objects::ObjectId>(id)));
            float sx = 0.0f, sy = 0.0f;
            if (ui::project(hf.world.view, {p.x, p.y, p.z}, sx, sy)) {
                hf.has_build_anchor = true;
                hf.build_anchor_px = {sx, sy};
            }
            break;
        }
    }
    hf.build_options = build_opts_;
    // Build queue: the local player's skirmish station production orders (queue order); index = the order's position
    // among the player's orders at its builder (the Production cancel index).
    hf.providers.build_queue = [sess, htypes](std::vector<ui::HudQueueEntry>& out) {
        const sim::TacticalBattle* b = sess ? sess->battle() : nullptr;
        if (!b || !b->production_enabled()) return;
        const auto orders = b->production()->queue(sess->local_player());
        for (size_t i = 0; i < orders.size(); ++i) {
            const tactical::hangar::BuildOrder& o = orders[i];
            if (o.type < 0) continue;
            uint8_t index = 0;
            for (size_t k = 0; k < i; ++k)
                if (orders[k].builder == o.builder) ++index;
            ui::HudQueueEntry e;
            e.type_index = static_cast<uint16_t>(o.type);
            e.icon = static_cast<size_t>(o.type) < htypes->size() ? (*htypes)[static_cast<size_t>(o.type)].icon_name : std::string();
            e.progress = o.total_frames > 0 ? 1.0f - static_cast<float>(o.frames_left) / static_cast<float>(o.total_frames) : 0.0f;
            e.location = static_cast<uint32_t>(o.builder);
            e.index = index;
            out.push_back(std::move(e));
        }
    };
#ifdef ALAMO_UI_HAVE_ABILITIES
    // Command bar ability buttons (special_button_NN) of the selection: S9 read-only queries between ticks.
    if (const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr)
        hf.providers.abilities = ui::s9_ability_provider(b->abilities(), [b] { return b->frame(); });
#endif
    hf.hardpoints = hud_hardpoints_;
    hf.reticles = &reticles_;
    hf.images = &hud_images_;
    // HUD_GAPS §5: order buttons from the local player's own selected units; story / Lua Lock_Controls disables all.
    {
        std::vector<OrderUnitState> st;
        const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
        for (const uint32_t id : sel) {
            const render::SnapshotObject* o = obj_of(static_cast<objects::ObjectId>(id));
            if (!o) continue;
            OrderUnitState u;
            u.owner = o->owner;
            u.can_attack = b && b->combat().can_attack(static_cast<tactical::combat::ObjectId>(id));
            u.can_move = b && b->movement().has_unit(static_cast<objects::ObjectId>(id));
            st.push_back(u);
        }
        hf.order_buttons = order_buttons(st, local_player(), story_controls_locked());
    }
    hud_units_ = units;
    // FIXPASS_GALACTIC §4.2: the advisor *_LOOP Bink in Movie_tactical (the HUD's own component), only while it plays.
    hf.advisor_page = advisor_loop_shown() ? static_cast<int32_t>(kAdvisorPage) : -1;
    hf.advisor_alpha = advisor_movie_.alpha();
    ui_.tactical_hud.build(hf, units, ui_.hud_types);
    if (ui_.tactical_hud.stats().reticles > 0) ++reticle_frames_;
}

// HUD_GAPS §1.1 / §2.2: the hovered unit = the bounding-sphere ray pick (pick_hud_unit, neutral objects skipped), replaced
// by the owner of the reticle under the cursor (the last build's reticles: what the player sees) so the set does not
// flicker; nothing over the HUD or a dialog. Then the hovered ship's hard-point reticles (§1.2: live targetable ones;
// destroyed ones get none) with their tint / texture inputs (§1.4 / §1.5) and flash start (§1.3).
void App::tactical_hover(ui::TacticalHudFrame& hf, std::vector<ui::HudUnit>& units) {
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    const bool over_ui = routed_.hover_component >= 0 || ui_.gui.hit(mx, my);
    uint32_t hover = 0;
    int32_t reticle = -1;
    if (!over_ui) {
        reticle = ui::reticle_at(ui_.tactical_hud.canvas(), hf.world.view, reticles_, hud_hardpoints_, static_cast<float>(mx),
                                 static_cast<float>(my), now_s_);
        if (reticle >= 0) hover = hud_hardpoints_[static_cast<size_t>(reticle)].parent;
        else {
            // Only objects with an encyclopedia identity (a HUD type with an Icon_Name): props such as the space
            // backdrops (Forest_Backdrop_Large: a huge bounding sphere owned by the neutral player) are never hovered.
            std::vector<ui::HudUnit> pickable;
            pickable.reserve(units.size());
            for (const ui::HudUnit& u : units)
                if (u.type < ui_.hud_types.size() && !ui_.hud_types[u.type].icon_name.empty()) pickable.push_back(u);
            if (const auto hit = ui::pick_hud_unit(pickable, hf.world.view, mx, my, true)) hover = pickable[*hit].id;
        }
    }
    const HardPointRef hovered_ref = reticle >= 0 ? hud_hp_refs_[static_cast<size_t>(reticle)] : HardPointRef{};
    hover_ship_ = hover;
    for (ui::HudUnit& u : units) u.hovered = hover != 0 && u.id == hover;
    hud_hardpoints_.clear();
    hud_hp_refs_.clear();
    const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
    if (!b || hover == 0 || !bview_.space()) return;
    const tactical::combat::CombatSystem& cs = b->combat();
    const auto id = static_cast<tactical::combat::ObjectId>(hover);
    const uint32_t n = cs.hardpoint_count(id);
    if (n == 0) return;
    const tactical::combat::CombatData& cd = cs.data();
    const uint32_t desc = cs.desc_index(id);
    const int32_t owner = cs.owner(id);
    const bool enemy = owner != local_player() && cs.is_enemy(local_player(), owner);
    ui::Vec3 pos = cs.position(id);
    for (const ui::HudUnit& u : units)
        if (u.id == hover) pos = u.position;  // the interpolated draw position
    const ui::Vec3 facing = cs.facing(id);
    for (uint32_t hp = 0; hp < n; ++hp) {
        if (!cs.hardpoint_targetable(id, hp)) continue;  // destroyed or not Is_Targetable (§1.2)
        const bool disabled = cs.hardpoint_disabled(id, hp);  // HARDPOINT_DISABLED.md: grey / Friendly_Disabled
        if (enemy && disabled) continue;                       // §1.2: no reticle for an enemy ship's disabled hard point
        ui::Vec3 offset{};
        if (desc < cd.units.size() && hp < cd.units[desc].hardpoints.size() && cd.units[desc].hardpoints[hp] < cd.hardpoints.size())
            offset = cd.hardpoints[cd.units[desc].hardpoints[hp]].attach_offset;
        const auto kind = static_cast<uint8_t>(cs.hardpoint_kind(id, hp));
        ui::HudHardPoint p;
        p.position = hardpoint_world(pos, facing, offset);
        p.type = hardpoint_type_name(kind);
        p.enemy = enemy;
        p.parent = hover;
        const float mx_hp = cs.hardpoint_max(id, hp);
        p.health = mx_hp > 0.0f ? std::clamp(cs.hardpoint_health(id, hp) / mx_hp, 0.0f, 1.0f) : 1.0f;
        p.disabled = disabled;
        p.repairing = !enemy && cs.hardpoint_repaired_by(id, hp, local_player());  // §1.3 blink: repaired by the local player
        p.tracked = hovered_ref.ship == hover && hovered_ref.index == hp && reticle >= 0;
        const auto f = hp_flash_.find((static_cast<uint64_t>(hover) << 8) | hp);
        p.flash_start = f != hp_flash_.end() ? f->second : -1.0;
        hud_hardpoints_.push_back(std::move(p));
        hud_hp_refs_.push_back({hover, hp, kind});
    }
}

// HUD_GAPS §1.6: a world order while the cursor is on a reticle targets the reticle's ship with that hard point - an
// attack carries ref2 = 1 + HardPointType (the sim's first targetable hard point of that type), a targeted ability its
// target; both flash the reticle. An own / allied damaged hard point that is not being repaired takes a right click as
// RepairHardpoint (command 45) when some unit of the local player's selection can repair hard points (HUD_GAPS §1.4:
// CombatSystem::can_repair_hardpoints, DUMMY_STAR_BASE). Returns true when the orders were rewritten.
bool App::reticle_orders(const ui::OrderContext& ctx, std::vector<commands::EventData>& cmds) {
    const ui::ButtonState& rb = input_.button(ui::Button::Right);
    const ui::ButtonState& lb = input_.button(ui::Button::Left);
    const bool rclick = rb.click && !rb.in_double_press;  // the release after a double press is swallowed (spec §1.1)
    if (!rclick && !lb.click) return false;
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    if (routed_.hover_component >= 0 || ui_.gui.hit(mx, my) || orders_.selection().empty()) return false;
    const int32_t r = ui::reticle_at(ui_.tactical_hud.canvas(), ctx.view, reticles_, hud_hardpoints_, static_cast<float>(mx),
                                     static_cast<float>(my), now_s_);
    if (r < 0) return false;
    const ui::HudHardPoint& hp = hud_hardpoints_[static_cast<size_t>(r)];
    const HardPointRef ref = hud_hp_refs_[static_cast<size_t>(r)];
    bool changed = false;
    if (rclick && selection_can_repair() && !hp.enemy && hp.health < 1.0f && !hp.repairing) {
        cmds.clear();
        commands::EventData e(commands::EventType::RepairHardpoint);
        e.set("object", ref.ship).set("hardpoint", ref.index).set("player", static_cast<uint32_t>(local_player()));
        cmds.push_back(std::move(e));
        ++reticle_orders_;
        return true;
    }
    for (commands::EventData& e : cmds) {
        if (e.type() == commands::EventType::Attack) {
            e.set("target", ref.ship).set("ref2", 1u + ref.kind);
            changed = true;
        } else if (e.type() == commands::EventType::TacticalSpecialAbility ||
                   e.type() == commands::EventType::TacticalSpecialAbilityWithDummyTarget) {
            e.set("target", ref.ship);
            changed = true;
        }
    }
    if (!changed && rclick && hp.enemy) {  // the translator saw no attack (cursor on the reticle, off the hull)
        std::erase_if(cmds, [](const commands::EventData& e) {
            using T = commands::EventType;
            const T t = e.type();
            return t == T::MoveToPosition || t == T::MoveToObject || t == T::MoveToPositionFacing || t == T::MoveToRay ||
                   t == T::MoveToRayFacing || t == T::Facing;
        });
        commands::EventData a = commands::ev::attack(static_cast<commands::ObjectId>(ref.ship), {});
        a.set("ref2", 1u + ref.kind);
        a.set_queued(input_.shift());
        cmds.push_back(std::move(a));
        changed = true;
    }
    if (changed) {
        hp_flash_[(static_cast<uint64_t>(ref.ship) << 8) | ref.index] = now_s_;
        ++reticle_orders_;
    }
    return changed;
}

// HUD_GAPS §1.4: OR of CombatSystem::can_repair_hardpoints over the local player's selected units.
bool App::selection_can_repair() const {
    const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
    if (!b) return false;
    for (const uint32_t u : orders_.selection()) {
        const auto uid = static_cast<tactical::combat::ObjectId>(u);
        if (b->combat().owner(uid) == local_player() && b->combat().can_repair_hardpoints(uid)) return true;
    }
    return false;
}

void App::hud_hit_dump(const char* where, int32_t component, int32_t px, int32_t py) {
    if (component == hud_hit_last_) return;
    hud_hit_last_ = component;
    const char* name = component >= 0 && static_cast<size_t>(component) < ui_.components.size() ? ui_.components[static_cast<size_t>(component)].name.c_str() : "-";
    std::printf("hud hit (%s): %d %s at %d,%d\n", where, component, name, px, py);
}

// ------------------------------------------------------------------------------------------------ render
bool App::present_settled() const {
    if (screen_ == Screen::Galactic) return gview_.settled() && draw_stats_.pending_pipeline == 0 && draw_stats_.translation_pending == 0;
    if (screen_ == Screen::Battle || screen_ == Screen::BattleLoad || (screen_ == Screen::Menu && demo_session() && bview_.active()))
        return bview_.settled(draw_stats_);
    return true;
}

bool App::render_frame() {
    if (!sc_.valid() || win_.minimized()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return true;
    }
    if (bview_.active()) bview_.pre_frame();  // fog uploads before this frame's acquire barriers
    rhi::Frame* f = gpu_.ring.begin_frame();
    if (!f) return false;
    uint32_t image = 0;
    const rhi::SwapchainStatus as = sc_.acquire(f->acquire_semaphore(), image);
    if (as == rhi::SwapchainStatus::OutOfDate || as == rhi::SwapchainStatus::Minimized || as == rhi::SwapchainStatus::Error) {
        gpu_.ring.end_frame(*f);
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
        return as != rhi::SwapchainStatus::Error;
    }
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = ctx_.vkd();
    const rhi::AcquireResult acq = gpu_.up.record_acquires(cmd);
    gpu_.renderer.set_time(static_cast<float>(now_s_));
    gview_.record_portraits(*f, cmd);  // planet portraits: before the main pass
    ui_.upload_pages(gpu_, gview_.portraits());
    const FrameTargets::Slot& slot = targets_.slots[f->slot() % targets_.slots.size()];
    const uint32_t w = targets_.width, h = targets_.height;
    render::PassTarget pt;
    pt.color = slot.color.handle;
    pt.color_view = slot.color.view;
    pt.extent = {w, h};
    pt.color_after = rhi::ImageState::TransferSrc;
    // World.
    bool ok = true;
    draw_stats_ = {};
    const bool galactic = screen_ == Screen::Galactic && gview_.attached();
    const bool battle = (screen_ == Screen::Battle || screen_ == Screen::BattleLoad || (screen_ == Screen::Menu && demo_session())) && bview_.active();
    const int world_view = galactic ? 1 : battle ? 2 : 0;
    if (world_view != world_view_) gpu_.renderer.reset_taa_history();
    world_view_ = world_view;
    apply_aa_options(gview_.ropt, adv_video_, video_profile_.anti_alias);  // AA combo + slider (t11)
    bview_.ropt.taa = gview_.ropt.taa;
    bview_.ropt.taa_weight = gview_.ropt.taa_weight;
    bview_.ropt.ambient_occlusion = adv_video_.ambient_occlusion;  // land battles only (render skips space / galactic)
    // The original's detail options on top of each view's own frame options (copies: the views keep theirs).
    render::RecordOptions gro = gview_.ropt, bro = bview_.ropt;
    if (detail_options_) {
        apply_detail_options(gro, adv_video_);
        apply_detail_options(bro, adv_video_);
        // t11: the settings with their own renderer entry points (texture mips, particle slots).
        gpu_.renderer.set_texture_reduction(texture_reduction(adv_video_.texture));
        gview_.set_particle_detail(particle_detail(adv_video_.particle));
        bview_.set_particle_detail(particle_detail(adv_video_.particle));
    }
    if (galactic) {
        const ui::CameraParams cp = gview_.camera_params(w, h);
        gview_.prepare(render_alpha(), now_s_, selected_planet_, &cp);
        gview_.use_scene(gpu_);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::to_view_matrices(cp, w, h), gview_.main, render::DrawMode::Lit, gro, &draw_stats_,
                                  nullptr, gview_.background);
    } else if (battle) {
        if (screen_ == Screen::Battle) bview_.set_selection_blobs(ui_.tactical_hud.selection_blobs());  // last HUD build's circles (one frame behind)
        else bview_.set_selection_blobs({});  // load screen / attract demo: no selection
        bview_.prepare(gpu_, render_alpha());
        bview_.use_scene(gpu_);
        const ui::CameraParams cp = bview_.camera_params(w, h);
        const float env_scale = detail_options_ ? environment_fade_scale(adv_video_.environment) : 1.0f;  // Environment detail [M]
        gpu_.renderer.set_object_fade(cp.fade_begin * env_scale, cp.fade_end * env_scale);
        for (int i = 0; i < 4; ++i) pt.clear[i] = bview_.clear_color(i);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::to_view_matrices(cp, w, h), bview_.main, render::DrawMode::Lit, bro, &draw_stats_,
                                  nullptr, bview_.sky);
    } else {
        render::RecordOptions ro;
        ro.shadows = false;
        ro.space = true;
        ro.post = false;
        gpu_.renderer.set_scene_hook(nullptr);
        ok = gpu_.renderer.render(*f, cmd, pt, ui::hud_ortho_view(ui_.gui.canvas(), ui::kHudCameraComponents), {}, render::DrawMode::Unlit, ro,
                                  &draw_stats_);
    }
    // HUD + dialogs.
    ui::HudDrawList hud_list, dialogs;
    const ui::HudCanvas* canvas = &ui_.gui.canvas();
    ui::ShellDrawer* drawer = nullptr;
    const ui::ShellSet* shells = nullptr;
    const ui::HudLayout* layout = nullptr;
    std::vector<ui::GalacticPlanetInfo> planets;
    if (galactic) {
        ui::GalacticHudFrame gf;
        build_galactic_hud(gf, planets);
        gf.advisor_page = advisor_loop_shown() ? static_cast<int32_t>(kAdvisorPage) : -1;  // FIXPASS_GALACTIC §4.2: Movie_galactic
        gf.advisor_alpha = advisor_movie_.alpha();
        ui_.galactic_hud.build(gf);
        // The shells the build wants shown (organiser, the two battle-prompt shells); re-applied to the layout on a change.
        bool shells_changed = false;
        for (const ui::GalacticShellVisibility& v : ui_.galactic_hud.shell_visibility())
            for (const ui::ShellSet::Shell& s : ui_.galactic_shells.shells())
                if (s.component == v.component && s.visible != v.visible) {
                    ui_.galactic_shells.set_visible(v.component, v.visible);
                    shells_changed = true;
                }
        if (shells_changed) ui_.galactic_shells.apply(ui_.galactic_hud.layout());
        hud_list = ui_.galactic_hud.draw_list();
        canvas = &ui_.galactic_hud.canvas();
        if (ui_.galactic_shells_ok) {
            drawer = &ui_.galactic_drawer;
            shells = &ui_.galactic_shells;
            layout = &ui_.galactic_hud.layout();
        }
    } else if (screen_ == Screen::Battle && battle) {
        build_tactical_hud();
        hud_list = ui_.tactical_hud.draw_list();
        canvas = &ui_.tactical_hud.canvas();
        if (ui_.tactical_shells_ok) {
            drawer = &ui_.tactical_drawer;
            shells = &ui_.tactical_shells;
            layout = &ui_.tactical_hud.layout();
        }
    }
    // HUD advisor loop: the galactic and battle HUDs draw it in Movie_galactic / Movie_tactical (FIXPASS_GALACTIC §4.2);
    // elsewhere 200 x 200 px, top-left [port].
    if (screen_ != Screen::Battle && !galactic && advisor_loop_shown()) {
        // STRATEGIC_GAPS §7.3: PrimAlpha (straight alpha) for a Movies.xml Alpha movie, PrimOpaque otherwise.
        ui::add_movie_quad(dialogs, 0, canvas->pixel_to_canvas(16.0f, 216.0f), canvas->pixel_to_canvas(216.0f, 16.0f), kAdvisorPage,
                           advisor_movie_.alpha());
    }
    if (full_movie_.has_frame() && (screen_ == Screen::Movie || full_movie_.playing())) {  // full-screen movie (2D scene layer 0)
        ui::HudQuad q;
        q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
        q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
        q.color = {255, 255, 255, 255};
        q.page = kMoviePage;
        dialogs.add(0, ui::HudList::Quads, q);
    }
    // Panels (hero roster, black market, sabotage, corruption, hack) on top of the HUD list; their shells draw at the
    // HUD's ShellModels step.
    std::vector<ui::HudDrawList> panel_lists;
    if (galactic || (screen_ == Screen::Battle && battle)) build_panels(panel_lists);
    for (const ui::HudDrawList& pl : panel_lists) merge_lists(hud_list, pl);
    if (galactic || (screen_ == Screen::Battle && battle)) build_encyclopedia(hud_list, *canvas);
    // OI host: the attract demo world under the menu. GuiManager's backdrop paints an opaque black fill under the
    // MenuBack_overlay quad, which would hide the world: left out while the demo is drawn [port: until engine/ui draws the
    // overlay alone - stretched to the window, straight alpha - over a world, MAIN_MENU_BACKGROUND.md §6.4].
    // MAIN_MENU_BACKGROUND.md §5 / §6.4: the overlay itself (title wordmark top, LucasArts / Petroglyph logos bottom left;
    // 86 % transparent) stays over the demo for as long as the menu shows: one quad stretched to the window, straight
    // alpha, under the dialogs, without the backdrop's black fill.
    std::string backdrop;
    if (battle && screen_ == Screen::Menu) {
        backdrop = ui_.gui.backdrop();
        ui_.gui.set_backdrop({});
        if (const ui::GuiImage img = ui_.gui.image(backdrop); img.valid) {
            ui::HudQuad q;
            q.min = {-0.5f * canvas->width, -0.5f * canvas->height};
            q.max = {0.5f * canvas->width, 0.5f * canvas->height};
            q.uv0 = img.uv0;
            q.uv1 = img.uv1;
            q.color = {255, 255, 255, 255};
            q.page = img.page;
            dialogs.add(0, ui::HudList::Alpha, q);
        }
    }
    ui_.gui.build(dialogs, now_s_);
    if (!backdrop.empty()) ui_.gui.set_backdrop(backdrop);
    if (fade_state_ != 0) {  // first main-menu open: black over the menu until the demo ticked 4 frames, then a 2 s fade
        const sim::TacticalBattle* fb = demo_session() ? session_->battle() : nullptr;
        if (!demo_session()) {
            fade_state_ = 0;
        } else if (fade_state_ == 1 && fb && fb->frame() >= 4) {
            fade_state_ = 2;
            fade_t0_ = now_s_;
        }
        const double alpha = fade_state_ == 1 ? 1.0 : 1.0 - (now_s_ - fade_t0_) / 2.0;
        if (fade_state_ == 2 && alpha <= 0.0) fade_state_ = 0;
        if (fade_state_ != 0) {
            ui::HudQuad q;
            q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
            q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
            q.color = {0, 0, 0, static_cast<uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5)};
            dialogs.add(ui::kHudLayers - 1, ui::HudList::Alpha, q);
        }
    }
    // Playtest QA D1: the end-of-battle hold ends in a fade to black over its last 2 s [port]
    if (const sim::TacticalBattle* hb = session_ ? session_->battle() : nullptr; hb && hb->end_hold_active()) {
        const float fade = 1.0f - std::min(static_cast<float>(hb->end_hold_frames_left()) / 60.0f, 1.0f);
        ui::HudQuad q;
        q.min = canvas->pixel_to_canvas(0.0f, static_cast<float>(h));
        q.max = canvas->pixel_to_canvas(static_cast<float>(w), 0.0f);
        q.color = {0, 0, 0, static_cast<uint8_t>(fade * 255.0f + 0.5f)};
        dialogs.add(ui::kHudLayers - 1, ui::HudList::Alpha, q);
    }
    info_.build_overlay(dialogs);  // data-chart lines over IDC_DATA_CHART_IMAGE
    ui::HudDrawList cursor_list;
    update_cursor(cursor_list);
    if (drawer) drawer->make_resident(gpu_.renderer);
    if (panel_shells_ok_) panel_drawer_.make_resident(gpu_.renderer);
    // Radar event models (tactical HUD only): uploaded the first time a ping names them, drawn at the Models2 step.
    std::span<const ui::HudModelDraw> radar_models;
    if (ui_.radar_models_ok && screen_ == Screen::Battle && battle && !galactic) {
        radar_models = ui_.tactical_hud.radar_models();
        std::string merr;
        if (!radar_models.empty() && !ui_.radar_drawer.prepare(gpu_.renderer, radar_models, &merr))
            std::fprintf(stderr, "radar event models: %s\n", merr.c_str());
        ui_.radar_drawer.make_resident(gpu_.renderer);
    }
    ui::HudTarget ht;
    ht.color = slot.color.handle;
    ht.color_view = slot.color.view;
    ht.depth = slot.depth.handle;
    ht.depth_view = slot.depth.view;
    ht.extent = {w, h};
    ht.before = ht.after = rhi::ImageState::TransferSrc;
    render::DrawStats shell_stats{};
    ok = gpu_.hud.record(
             *f, cmd, ht, *canvas, hud_list, ui_.fonts.get(),
             [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                 if (step.kind == ui::HudStepKind::Models2 && !radar_models.empty()) {
                     render::DrawStats s3;
                     ui_.radar_drawer.draw(gpu_.renderer, *f, c, *canvas, radar_models, &s3);
                     shell_stats.pending_pipeline += s3.pending_pipeline;
                 }
                 if (step.kind == ui::HudStepKind::ShellModels && drawer && shells && layout) {
                     render::DrawStats s1;
                     drawer->draw(gpu_.renderer, *f, c, *canvas, *shells, *layout, step.first, &s1);
                     shell_stats.pending_pipeline += s1.pending_pipeline;
                 }
                 if (step.kind == ui::HudStepKind::ShellModels && panel_shells_ok_ && !panel_lists.empty()) {
                     for (ui::HudPanel* p : panels())
                         if (p->open()) {
                             render::DrawStats s2;
                             panel_drawer_.draw(gpu_.renderer, *f, c, p->canvas(), panel_shells_, p->layout(), step.first, &s2);
                         }
                 }
             },
             nullptr, &dialogs, cursor_ok_ ? &cursor_list : nullptr) &&
         ok;
    draw_stats_.pending_pipeline += shell_stats.pending_pipeline;
    // Options > Video > Gamma over the finished frame (world + HUD), 50 = identity (t11).
    if (detail_options_ && ok) {
        const float gamma = gamma_value(video_profile_.gamma);
        (void)gpu_.renderer.apply_gamma(*f, cmd, slot.color.handle, slot.color.view, slot.color.vk_format(), {w, h},
                                        rhi::ImageState::TransferSrc, rhi::ImageState::TransferSrc, gamma);  // a failure logs; the frame goes out ungammed
    }
    // Present: blit the frame into the swapchain image; optional readback for a PNG.
    const VkImage sw = sc_.image(image);
    rhi::cmd_barrier(fn, cmd, rhi::transition(sw, rhi::ImageState::Undefined, rhi::ImageState::TransferDst));
    VkImageBlit2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    region.dstOffsets[1] = {static_cast<int32_t>(sc_.extent().width), static_cast<int32_t>(sc_.extent().height), 1};
    VkBlitImageInfo2 bi{};
    bi.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    bi.srcImage = slot.color.handle;
    bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bi.dstImage = sw;
    bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bi.regionCount = 1;
    bi.pRegions = &region;
    bi.filter = VK_FILTER_LINEAR;
    fn.vkCmdBlitImage2(cmd, &bi);
    rhi::cmd_barrier(fn, cmd, rhi::transition(sw, rhi::ImageState::TransferDst, rhi::ImageState::Present));
    const bool readback = !shot_path_.empty();
    if (readback) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        fn.vkCmdCopyImageToBuffer(cmd, slot.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets_.readback.handle, 1, &copy);
        const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(targets_.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                              VK_ACCESS_2_HOST_READ_BIT);
        rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    }
    VkSemaphoreSubmitInfo waits[2];
    uint32_t nw = 0;
    waits[nw++] = rhi::semaphore_info(f->acquire_semaphore(), 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (acq.wait_value) waits[nw++] = rhi::semaphore_info(gpu_.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    const VkSemaphoreSubmitInfo signal = rhi::semaphore_info(sc_.render_finished(image), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    fs.waits = {waits, nw};
    fs.signals = {&signal, 1};
    if (!gpu_.ring.end_frame(*f, fs)) return false;
    const rhi::SwapchainStatus ps = sc_.present(image);
    if (ps == rhi::SwapchainStatus::OutOfDate || ps == rhi::SwapchainStatus::Suboptimal || as == rhi::SwapchainStatus::Suboptimal) {
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
    } else if (ps == rhi::SwapchainStatus::Error) {
        return false;
    }
    if (readback) {
        gpu_.ring.wait_idle();
        gpu_.alloc.invalidate(targets_.readback.alloc);
        const auto* px = static_cast<const uint8_t*>(targets_.readback.mapped());
        std::vector<uint8_t> rgba(px, px + static_cast<size_t>(w) * h * 4);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        const bool wrote = render::write_png_rgba(shot_path_, w, h, rgba);
        std::printf("%s %s (%s: %u draws, %u instances)\n", wrote ? "wrote" : "FAILED", shot_path_.c_str(), screen_name(screen_), draw_stats_.draws,
                    draw_stats_.instances);
        if (!wrote) smoke_ok_ = false;
        shot_path_.clear();
        shot_done_ = true;
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------ frame
void App::frame() {
    // Scripted runs are frame-driven (30 frames per presentation second): camera smoothing, double-tap windows and
    // pop-up delays then advance identically on every machine, so the recorded command stream is machine independent.
    const double now = script_.empty() ? ms_since(t0_) / 1000.0 : static_cast<double>(frames_) / 30.0;
    dt_ = static_cast<float>(std::clamp(now - now_s_, 0.0, 0.1));
    now_s_ = now;
    if (win_.take_resized() && win_.width() > 0 && win_.height() > 0) {
        gpu_.ring.wait_idle();
        sc_.recreate(win_.width(), win_.height());
        targets_.destroy(gpu_.alloc);
        targets_.create(gpu_.alloc, gpu_.ring.frames_in_flight(), win_.width(), win_.height());
        ui_.resize(win_.width(), win_.height());
    }
    // 1. Input (script frames replace the window in smoke runs).
    ui::InputFrame raw;
    if (!script_.empty()) {
        raw = idle_frame();
        if (script_step_ < script_.size()) {
            if (hold_release_) hold_sim_ = hold_release_ = false;
            ScriptStep& s = script_[script_step_];
            // Story dialogs fire whenever the plots say so: outside the story steps the script closes one after 30 frames.
            if (ui_.gui.find(ui::dlg::kStory) && s.name.find("story") == std::string::npos) {
                if (++story_dialog_frames_ > 30) {
                    ui_.gui.close(ui::dlg::kStory);
                    std::printf("script: story dialog closed\n");
                }
            } else {
                story_dialog_frames_ = 0;
            }
            if (s.run(*this, raw)) {
                std::printf("script: %s done (frame %" PRIu64 ", %s, tick %" PRIu64 ")\n", s.name.c_str(), frames_, screen_name(screen_),
                            session_ ? session_->host().frame() : 0);
                ++script_step_;
                step_frames_ = 0;
            } else if (++step_frames_ > s.timeout_frames) {
                std::fprintf(stderr, "script: step '%s' timed out (%s); dialogs:", s.name.c_str(), screen_name(screen_));
                for (const std::string& n : ui_.gui.open_names()) std::fprintf(stderr, " %s", n.c_str());
                std::fprintf(stderr, "\n");
                script_failed_ = true;
                screen_ = Screen::Quit;
                return;
            }
        }
        (void)ui::capture_input(win_, now_s_, dt_);  // consume the window's edges
    } else {
        raw = ui::capture_input(win_, now_s_, dt_);
    }
    std::u32string typed = printable(win_.take_text());
    // OI host: attract demo (idle clock, idle pause, map cycle); input reaches the menus as usual.
    attract_step(raw);
    // 2. Routing: overlay -> dialogs -> HUD -> world.
    ui::RouteHooks hooks;
    hooks.dialog_hit = [this](int32_t x, int32_t y) { return ui_.gui.hit(x, y); };
    hooks.dialog_focus = [this] { return ui_.gui.has_focus(); };
    // A running targeted-ability pick (not under a dialog or the pause menu) takes the presses before dialogs and the HUD.
    hooks.targeting_active = [this] { return screen_ == Screen::Battle && targeting_.active() && !paused_ && !ui_.gui.has_focus(); };
    // HUD hit test: open panels first, then the galactic / tactical HUD (hud_panels.hpp host wiring).
    auto panel_hit = [this](int32_t x, int32_t y) {
        for (ui::HudPanel* p : panels())
            if (p->open())
                if (const int32_t c = p->hit_test(x, y); c >= 0) return c;
        return -1;
    };
    if (screen_ == Screen::Galactic)
        hooks.hud_hit = [this, panel_hit](int32_t x, int32_t y) {
            const int32_t c = panel_hit(x, y);
            return c >= 0 ? c : ui_.galactic_hud.hit_test(x, y);
        };
    if (screen_ == Screen::Galactic)  // right presses on the planet fleet / land-forces buttons reach the planet picking
        hooks.hud_takes = [this, panel_hit](int32_t c, ui::Button b) {
            if (panel_hit(input_.mouse_x(), input_.mouse_y()) >= 0) return true;  // an open panel's component
            return ui_.galactic_hud.takes_button(c, b);
        };
    else if (screen_ == Screen::Battle)
        hooks.hud_hit = [this, panel_hit](int32_t x, int32_t y) {
            const int32_t c = panel_hit(x, y);
            return c >= 0 ? c : ui_.tactical_hud.hit_test(x, y);
        };
    router_.route(raw, hooks, routed_);
    target_input_.update(raw);
    gui_events_.clear();
    ui_.gui.update(raw, routed_, now_s_, gui_events_, typed);
    std::vector<ui::MenuAction> acts;
    ui_.front.update(gui_events_, raw, acts);
    handle_menu_actions(acts);
    {  // in-game screens: battle end, retreat, planet info, buy units, story, holocron (P4)
        std::vector<ui::ScreenAction> sacts;
        std::vector<commands::EventData> scmds;
        screens_.update(gui_events_, sacts, scmds);
        info_.update(gui_events_, sacts);
        handle_screen_actions(sacts);
        for (commands::EventData& e : scmds) submit(std::move(e));  // e.g. the confirmed Retreat
    }
    if (ui_.load.is_open()) {
        std::vector<ui::LoadScreenEvent> lev;
        ui_.load.update(gui_events_, lev);
        for (const ui::LoadScreenEvent& e : lev)
            if (e.kind == ui::LoadScreenEvent::Kind::Begin && screen_ == Screen::BattleLoad) {
                screen_ = Screen::Battle;
                audio_.music(MusicScene::Battle, local_faction(), bview_.space());
            }
    }
    input_.update(routed_.world);
    if (screen_ == Screen::Movie) movie_step();
    if (session_ && !paused_ && (screen_ == Screen::Galactic || screen_ == Screen::Battle) && routed_.keys_to_world &&
        input_.key_pressed(ui::vk::kEscape) && script_.empty() && !(screen_ == Screen::Battle && targeting_.active())) {
        paused_ = true;  // (Esc during a targeted-ability pick only cancels the pick: battle_input)
        ui_.front.show_pause_menu();
    }
    // MAIN_LOOP.md §1.5: the pause menu and the full-screen dialogs it opens (options and their pages, save / load, quit
    // confirm) set the tactical pause in single player - no ticks while any of them is open, whatever opened it.
    if (session_ && !demo_session() && (screen_ == Screen::Galactic || screen_ == Screen::Battle) && menu_dialog_open()) paused_ = true;
    if (paused_ && !menu_dialog_open()) paused_ = false;
    if (paused_ || ui_.gui.has_focus()) targeting_.cancel();  // a pick does not outlive the pause menu / a dialog
    if (session_ && !paused_) {
        if (screen_ == Screen::Galactic) galactic_input();
        else if (screen_ == Screen::Battle) battle_input();
    }
    // The space reinforcement grid only while a drop point is being picked (battle_input does not run while paused or
    // outside the battle screen, so the flag is kept here too).
    if (bview_.active()) bview_.set_space_reinforcement_overlay(screen_ == Screen::Battle && bview_.space() && reinforce_unit_ != 0);
    // 3. Sim, then the story's presentation ops (movies, audio) of the ticks just run; Lua movies advance.
    if (battle_view_pending_ && load_intro_frames_ == 0) start_battle_view();  // QA t11: the load screen is already up
    step_sim();
    if (screen_ == Screen::Menu && demo_session()) {  // OI host: the attract demo's camera; a finished demo battle restarts
        if (session_->skirmish_over() && !start_attract_map()) stop_attract();
        else demo_camera();
    }
    lua_presentation();
    story_reward_step();  // L1/S4 wave 1: START_MOVIE / MULTIMEDIA reward media
    if (screen_ == Screen::Galactic && session_) galactic_screens();
    if (screen_ != Screen::Movie) {
        if (full_movie_.playing() && !full_movie_.update(dt_, gpu_.hud, kMoviePage)) {
            story_ui_.movie_done(full_movie_.name());  // L1/S4 wave 1: STORY_MOVIE_DONE (an in-game full-screen movie ended)
            full_movie_.stop(audio_);
        }
        if (advisor_movie_.playing()) {
            const uint16_t page = story_movie_ && story_page_ >= 0 ? static_cast<uint16_t>(story_page_) : kAdvisorPage;
            advisor_movie_.update(dt_, gpu_.hud, page);
            if (page != kAdvisorPage && advisor_movie_.has_frame()) ui_.pages.set_loaded(page, advisor_movie_.width(), advisor_movie_.height());
        }
    }
    story_ui_step();  // L1/S4 wave 1: this frame's UI notifications for the story
    // 4. Render, 5. audio (listener from the camera).
    if (screen_ == Screen::Quit) return;
    if (!render_frame()) {
        std::fprintf(stderr, "render failed\n");
        smoke_ok_ = false;
        screen_ = Screen::Quit;
        return;
    }
    end_load_intro_frame();
    const bool cinematic = presentation_cinematic();  // OI host: Lua cinematic camera -> listener rules
    if (screen_ == Screen::Galactic) {
        const ui::CameraParams cp = gview_.camera_params(targets_.width, targets_.height);
        audio_.set_listener({cp.eye.x, cp.eye.y, cp.eye.z}, {cp.target.x, cp.target.y, cp.target.z}, cinematic);
    } else if ((screen_ == Screen::Battle || (screen_ == Screen::Menu && demo_session())) && bview_.active()) {
        const ui::CameraParams cp = bview_.camera_params(targets_.width, targets_.height);
        audio_.set_listener({cp.eye.x, cp.eye.y, cp.eye.z}, {cp.target.x, cp.target.y, cp.target.z}, cinematic);
    }
    audio_flags();  // OI host: modal / GUI-only / attract
    audio_.update(dt_);
    ++frames_;
    if (frames_ % 60 == 0 && !opt_.hidden) {
        char title[256];
        std::snprintf(title, sizeof title, "Alamo - %s - tick %" PRIu64 ", %u draws", screen_name(screen_), session_ ? session_->host().frame() : 0,
                      draw_stats_.draws);
        win_.set_title(title);
    }
}

// ------------------------------------------------------------------------------------------------ presentation on/off check
bool App::crc_check() {
    if (!session_) {
        std::fprintf(stderr, "crc check: no session\n");
        return false;
    }
    const CrcLog& on = session_->host().crc_log();
    const uint64_t ticks = session_->ticks();
    CrcLog off;
    if (session_->loaded()) {  // N1 / t09: the same save loaded again, then the stream recorded after the load
        jobs::JobSystem js(jobs::JobSystemConfig{opt_.workers});
        std::string err;
        std::unique_ptr<GameSession> replay = load_game(data_, js, loaded_save_, &err, &recording_);
        if (!replay) {
            std::fprintf(stderr, "replay: %s\n", err.c_str());
            return false;
        }
        for (uint64_t i = 0; i < ticks; ++i) replay->tick();
        off = replay->host().crc_log();
    } else {
        jobs::JobSystem js(jobs::JobSystemConfig{opt_.workers});
        GameSession replay(data_, js, session_->spec());
        std::vector<std::string> diag;
        if (!replay.start(&diag, &recording_, nullptr)) {
            for (const std::string& d : diag) std::fprintf(stderr, "replay: %s\n", d.c_str());
            return false;
        }
        for (uint64_t i = 0; i < ticks; ++i) replay.tick();
        off = replay.host().crc_log();
    }
    if (!opt_.png_dir.empty()) {  // the run's CRC log and command stream, for cross-build comparisons
        std::string e;
        const std::string base = opt_.png_dir + "/game_smoke_" + opt_.smoke;
        if (!on.save(base + ".crc", &e) || !recording_.save(base + ".rec", &e)) std::fprintf(stderr, "crc / recording save: %s\n", e.c_str());
    }
    const CrcDivergence d = first_divergence(on, off);
    std::printf("presentation on vs off (replay of %zu recorded batches): %zu vs %zu records, final %016" PRIx64 " vs %016" PRIx64 ": %s\n",
                recording_.batches.size(), on.records.size(), off.records.size(), final_crc(on), final_crc(off), d.message.c_str());
    return !d.diverged && on.records.size() == off.records.size() && !on.records.empty();
}

// ------------------------------------------------------------------------------------------------ script
ui::InputFrame App::idle_frame() const {
    ui::InputFrame f;
    f.time = static_cast<double>(frames_) / 30.0;
    f.dt = 1.0f / 30.0f;
    f.width = targets_.width;
    f.height = targets_.height;
    f.mouse_x = static_cast<int32_t>(targets_.width / 2);
    f.mouse_y = static_cast<int32_t>(targets_.height / 2);
    f.mouse_inside = false;  // no edge scrolling
    return f;
}

// A click as three frames: move, press, release (InputState: a click without a drag).
struct ClickState {
    int phase = 0;
};
bool click_at(ClickState& c, ui::InputFrame& f, int32_t x, int32_t y, ui::Button b, bool shift = false) {
    if (shift) f.keys_down.set(ui::vk::kShift);  // the queue modifier held through the click
    f.mouse_x = x;
    f.mouse_y = y;
    f.mouse_inside = true;
    const uint8_t bit = ui::button_bit(b);
    if (c.phase == 1) {
        f.buttons_down = bit;
        f.buttons_pressed = bit;
    } else if (c.phase == 2) {
        f.buttons_released = bit;
    }
    return ++c.phase > 3;
}

ScriptStep step_wait(std::string name, std::function<bool(App&)> cond, uint32_t timeout = 3600) {
    return {std::move(name), [cond = std::move(cond)](App& a, ui::InputFrame&) { return cond(a); }, timeout};
}

ScriptStep step_click_control(std::string dialog, std::string control) {
    auto st = std::make_shared<ClickState>();
    std::string name = "click " + dialog + "/" + control;
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    return {name, [st, px, dialog, control](App& a, ui::InputFrame& f) {
                if (st->phase > 0) return click_at(*st, f, px->first, px->second, ui::Button::Left);  // the dialog may close on release
                ui::GuiDialog* d = a.ui_.gui.find(dialog);
                if (!d || d != a.ui_.gui.top()) return false;  // wait for the dialog
                const int32_t i = d->find(control);
                if (i < 0) {
                    std::fprintf(stderr, "script: %s has no %s\n", dialog.c_str(), control.c_str());
                    a.script_failed_ = true;
                    return true;
                }
                const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
                *px = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
                return click_at(*st, f, px->first, px->second, ui::Button::Left);
            },
            600};
}

// Selects list row `row` like a player: click the first row (selects + focuses the list), then Down until it is reached.
ScriptStep step_select_row(std::string dialog, std::string control, std::function<size_t(App&)> row) {
    auto st = std::make_shared<ClickState>();
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    auto key_frame = std::make_shared<bool>(false);
    return {"select row of " + dialog + "/" + control, [st, px, key_frame, dialog, control, row](App& a, ui::InputFrame& f) {
                ui::GuiDialog* d = a.ui_.gui.find(dialog);
                if (!d) return false;
                const ui::GuiControl* c = d->get(control);
                if (!c) return false;
                if (st->phase == 0) {
                    const int32_t i = d->find(control);
                    const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                    const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), r.max.y - 6.0f);
                    *px = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)};
                }
                if (st->phase <= 3) {
                    click_at(*st, f, px->first, px->second, ui::Button::Left);
                    return false;
                }
                const size_t want = row(a);
                if (c->selected >= 0 && static_cast<size_t>(c->selected) >= want) return true;
                *key_frame = !*key_frame;
                if (*key_frame) {
                    f.keys_down.set(ui::vk::kDown);
                    f.keys_pressed.set(ui::vk::kDown);
                }
                return false;
            },
            2000};
}

ScriptStep step_shot(std::string file) {
    auto st = std::make_shared<int>(0);
    return {"shot " + file, [st, file](App& a, ui::InputFrame&) {
                if (a.opt_.png_dir.empty()) return true;
                if (*st == 0) {  // hold the sim while models stream and pipelines compile (presentation-only pause)
                    a.hold_sim_ = true;
                    a.shot_done_ = false;
                    *st = 1;
                    return false;
                }
                if (*st < 1000 && !(a.present_settled() && *st > 3)) {
                    ++*st;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    return false;
                }
                if (a.shot_path_.empty() && !a.shot_done_) {
                    if (!a.present_settled()) a.notes_.push_back(file + ": taken before everything settled" + std::string(a.gview_.settled() ? " (models ok" : " (models pending") + ", pipelines " + std::to_string(a.draw_stats_.pending_pipeline) + ", translation " + std::to_string(a.draw_stats_.translation_pending) + ")");
                    a.shot_path_ = a.opt_.png_dir + "/" + file;
                    return false;
                }
                if (!a.shot_done_) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

ScriptStep step_ticks(std::string name, uint64_t n) {
    auto start = std::make_shared<uint64_t>(~uint64_t{0});
    return {std::move(name), [start, n](App& a, ui::InputFrame&) {
                if (!a.session_) return false;
                if (*start == ~uint64_t{0}) *start = a.session_->ticks();
                return a.session_->ticks() >= *start + n;
            },
            20000};
}

// One key press (a frame with the key down and pressed, then idle frames).
ScriptStep step_key(std::string name, uint16_t key) {
    auto st = std::make_shared<int>(0);
    return {std::move(name), [st, key](App&, ui::InputFrame& f) {
                if (*st == 1) {
                    f.keys_down.set(key);
                    f.keys_pressed.set(key);
                }
                return ++*st > 3;
            },
            100};
}

// Pause (the Pause hotkey, slot 153 -> pause_event in the command stream): once the entry drained no tick runs while
// rendering continues; then resume. Checks the sim frame stands still for 20 frames.
void add_pause_steps(std::vector<ScriptStep>& s) {
    s.push_back(step_key("pause key", ui::vk::kPause));
    s.push_back(step_wait("paused", [](App& a) { return a.session_ && a.session_->paused(); }, 300));
    auto frame0 = std::make_shared<uint64_t>(0);
    auto n = std::make_shared<int>(0);
    s.push_back({"sim stands still while paused", [frame0, n](App& a, ui::InputFrame&) {
                     if (*n == 0) *frame0 = a.session_->host().frame();
                     if (++*n < 20) return false;
                     if (a.session_->host().frame() != *frame0) {
                         std::fprintf(stderr, "script: the sim ticked while paused (%" PRIu64 " -> %" PRIu64 ")\n", *frame0, a.session_->host().frame());
                         a.script_failed_ = true;
                     }
                     return true;
                 },
                 100});
    s.push_back(step_key("resume key", ui::vk::kPause));
    s.push_back(step_wait("resumed", [](App& a) { return a.session_ && !a.session_->paused() && !a.session_->pause_requested(); }, 300));
}

// A hotkey slot pressed with its current binding (key + modifier from the HotkeyTable).
ScriptStep step_hotkey(std::string name, uint16_t slot) {
    auto st = std::make_shared<int>(0);
    return {std::move(name), [st, slot](App& a, ui::InputFrame& f) {
                const ui::HotkeyBinding b = a.ui_.hotkeys.binding(slot);
                if (b.key < 0) {
                    std::fprintf(stderr, "script: hotkey slot %u is unbound\n", slot);
                    a.script_failed_ = true;
                    return true;
                }
                if (*st == 1) {
                    if (b.mod == ui::HotkeyMod::Ctrl) f.keys_down.set(ui::vk::kControl);
                    if (b.mod == ui::HotkeyMod::Shift) f.keys_down.set(ui::vk::kShift);
                    if (b.mod == ui::HotkeyMod::Alt) f.keys_down.set(ui::vk::kAlt);
                    f.keys_down.set(static_cast<uint16_t>(b.key));
                    f.keys_pressed.set(static_cast<uint16_t>(b.key));
                }
                return ++*st > 3;
            },
            100};
}


// Battle: control group 1 = the selection (Ctrl+1), then '1' twice within a second: the recall plus the camera jump to the
// group's centroid (SimHost::control_group through CameraFocus::group).
void add_control_group_steps(std::vector<ScriptStep>& s) {
    s.push_back({"assign control group 1 (Ctrl+1)", [](App&, ui::InputFrame& f) {
                     static int n = 0;
                     if (++n == 2) {
                         f.keys_down.set(ui::vk::kControl);
                         f.keys_down.set(ui::vk::key('1'));
                         f.keys_pressed.set(ui::vk::key('1'));
                     }
                     return n > 4;
                 },
                 100});
    s.push_back(step_wait("group 1 assigned", [](App& a) {
        return a.session_ && !a.session_->host().control_group(static_cast<uint8_t>(a.local_player()), 1).empty();
    }, 300));
    s.push_back({"recall group 1 twice (camera jump)", [](App& a, ui::InputFrame& f) {
                     static int n = 0;
                     ++n;
                     if (n == 1) a.bview_.camera().jump_to({0.0f, 0.0f, 0.0f});
                     if (n == 3 || n == 6) {
                         f.keys_down.set(ui::vk::key('1'));
                         f.keys_pressed.set(ui::vk::key('1'));
                     }
                     if (n < 20) return false;
                     const std::vector<objects::ObjectId>& g = a.session_->host().control_group(static_cast<uint8_t>(a.local_player()), 1);
                     double x = 0, y = 0;
                     uint32_t k = 0;
                     for (const render::SnapshotObject& o : a.bview_.frame().objects)
                         if (std::binary_search(g.begin(), g.end(), o.id)) {
                             x += o.position.x;
                             y += o.position.y;
                             ++k;
                         }
                     const ui::CameraParams cp = a.bview_.camera_params(a.targets_.width, a.targets_.height);
                     const double dx = cp.target.x - (k ? x / k : 0.0), dy = cp.target.y - (k ? y / k : 0.0);
                     std::printf("script: control group 1: %zu units, camera %.0f units from their centroid\n", g.size(), std::sqrt(dx * dx + dy * dy));
                     if (k == 0 || dx * dx + dy * dy > 400.0 * 400.0) {
                         std::fprintf(stderr, "script: the camera did not jump to control group 1\n");
                         a.script_failed_ = true;
                     }
                     return true;
                 },
                 100});
}

// Encyclopedia: the camera looks at a world object (battle: a local unit, galactic: a planet) so it sits under the
// cursor at the screen centre (where the idle script frames keep it, through the following shot too); done once the
// pop-up was drawn for a few frames.
ScriptStep step_encyclopedia(std::string name) {
    auto aimed = std::make_shared<bool>(false);
    auto shown0 = std::make_shared<uint64_t>(~uint64_t{0});
    return {std::move(name), [aimed, shown0](App& a, ui::InputFrame&) {
                // The pop-up's delay is wall time: the sim is held meanwhile (presentation-only) so the ticks of the
                // run stay independent of the machine's speed (the CRC log compares across compilers).
                a.hold_sim_ = true;
                if (*shown0 == ~uint64_t{0}) *shown0 = a.ency_shown_;
                if (!*aimed) {
                    if (a.screen_ == Screen::Battle && a.bview_.active()) {
                        for (const render::SnapshotObject& o : a.bview_.frame().objects)
                            if (o.owner == a.local_player() && a.session_ && a.session_->battle() && a.session_->battle()->movement().has_unit(o.id)) {
                                a.bview_.camera().jump_to({o.position.x, o.position.y, o.position.z});
                                *aimed = true;
                                break;
                            }
                    } else if (a.screen_ == Screen::Galactic) {
                        for (const ui::PickSphere& s : a.gview_.pick_items())
                            if (s.kind == ui::PickKind::Planet) {
                                a.gview_.camera().set_look_at(s.center);
                                *aimed = true;
                                break;
                            }
                    }
                    return false;
                }
                if (a.ency_shown_ <= *shown0 + 2) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

// Screen smokes (opt-in: ALAMO_SMOKE_SCREENS=1, so the default smoke stays as it is; with ALAMO_PSEUDO_LOC=1 they show
// overflow, QA U2 / U4). None sends a command or moves a tick: the CRC log matches the default run's.
// Unit tooltip: the visible unit with the most tooltip text under the cursor with the sim held, `file` when the pop-up is up;
// the camera goes back where it was.
ScriptStep step_unit_tooltip(std::string file) {
    struct State {
        int phase = 0;
        uint32_t target = 0, n = 0;
        ui::Vec3 look{};
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"unit tooltip", [st](App& a, ui::InputFrame& f) {
                if (a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                a.hold_sim_ = true;
                if (st->phase == 0) {  // aim: the cursor stays in the middle of the screen
                    size_t most = 0;
                    ui::Vec3 at{};
                    for (const ui::HudUnit& u : a.hud_units_) {
                        if (u.type >= a.data_.types.size()) continue;
                        const ui::EncyclopediaEntry e = encyclopedia_entry(a.data_, a.ui_.text, a.ui_.hud_types, u.type);
                        size_t len = e.header.size() + e.description.size();
                        for (const ui::EncyclopediaRow& r : e.rows) len += r.left.size() + r.right.size() + r.center.size();
                        if (len > most) {
                            most = len;
                            st->target = u.id;
                            at = u.position;
                        }
                    }
                    if (st->target == 0) {
                        a.notes_.push_back("unit tooltip: no unit on the HUD");
                        a.hold_release_ = true;
                        return true;
                    }
                    st->look = a.bview_.camera().look_at();
                    a.bview_.camera().jump_to(at);
                    st->phase = 1;
                    return false;
                }
                if (st->phase == 1) {
                    if (a.ency_.shown_object() != st->target && ++st->n <= 150) return false;
                    if (a.ency_.shown_object() != st->target) a.notes_.push_back("unit tooltip: another object under the cursor");
                    st->phase = 2;
                    return false;
                }
                if (!st->shot.run(a, f)) return false;
                a.bview_.camera().jump_to(st->look);
                a.hold_sim_ = true;
                a.hold_release_ = true;  // no ticks on this frame: the next step runs when it would have without this one
                return true;
            },
            3000};
}

// Victory screen: over the open battle-end screen, the real type names with the longest labels and large counts (a smoke
// battle loses few units, QA U2 needs "Skipray Blastboat x12"), 14 rows on your side so its scrollbar shows too. Continue closes
// it as usual: the screen's data is display only.
ScriptStep step_victory_screen(std::string file) {
    struct State {
        bool shown = false;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"victory screen", [st](App& a, ui::InputFrame& f) {
                if (st->shown) return st->shot.run(a, f);
                std::vector<std::pair<std::u32string, objects::TypeIndex>> names;
                for (objects::TypeIndex t = 0; t < a.data_.types.size() && t < a.ui_.hud_types.size(); ++t)
                    if (const std::string_view id = a.data_.types.get_string(t, "Text_ID");
                        !id.empty() && a.ui_.text.find(id) && !a.ui_.hud_types[t].icon_name.empty())
                        names.emplace_back(type_label(a.data_, a.ui_.text, t), t);
                std::stable_sort(names.begin(), names.end(), [](const auto& x, const auto& y) { return x.first.size() > y.first.size(); });
                ui::BattleEndInfo info;
                info.victory = true;
                info.planet = a.battle_place_;
                info.battle_seconds = 3599;
                info.bounty = 123456;
                info.your_loss_value = 1234567;
                info.enemy_loss_value = 7654321;
                info.your_efficiency = 100;
                info.enemy_efficiency = 100;
                info.can_save_replay = false;
                for (size_t i = 0; i < names.size() && i < 20; ++i) {
                    ui::BattleEndUnit u;
                    u.name = names[i].first;
                    u.icon = a.ui_.hud_types[names[i].second].icon_name;
                    u.count = i % 2 ? 12 : 1;
                    (i < 14 ? info.your_losses : info.enemy_losses).push_back(std::move(u));
                }
                if (!a.screens_.show_battle_end(info)) a.notes_.push_back("victory screen: dialog missing");
                st->shown = true;
                return false;
            },
            1400};
}
// Ctrl+Y (the SelectAll hotkey slot's default).
ScriptStep step_select_all() {
    auto st = std::make_shared<int>(0);
    return {"select all", [st](App&, ui::InputFrame& f) {
                if (*st == 1) {
                    f.keys_down.set(ui::vk::kControl);
                    f.keys_down.set(ui::vk::key('Y'));
                    f.keys_pressed.set(ui::vk::key('Y'));
                }
                return ++*st > 3;
            },
            100};
}

// Battle: aim the camera at the nearest enemy unit, then right-click it (Attack).
ScriptStep step_attack_enemy(bool shift = false) {
    auto st = std::make_shared<ClickState>();
    auto target = std::make_shared<uint32_t>(0);
    auto px = std::make_shared<std::pair<int32_t, int32_t>>(0, 0);
    auto settle = std::make_shared<int>(0);
    return {shift ? "shift+right-click an enemy" : "right-click an enemy", [st, target, px, settle, shift](App& a, ui::InputFrame& f) {
                if (!a.session_ || !a.bview_.active() || a.screen_ != Screen::Battle) return false;
                const render::InterpolatedFrame& fr = a.bview_.frame();
                const render::SnapshotObject* t = nullptr;
                for (const render::SnapshotObject& o : fr.objects)
                    if (*target == 0 ? (o.owner >= 0 && o.owner != a.local_player() && (o.flags & render::kSnapDying) == 0 &&
                                        a.session_->battle() && a.session_->battle()->combat().has_unit(o.id) &&
                                        a.session_->battle()->combat().is_enemy(a.local_player(), o.owner))
                                     : static_cast<uint32_t>(o.id) == *target) {
                        t = &o;
                        break;
                    }
                if (!t) return false;
                if (*target == 0) {
                    *target = static_cast<uint32_t>(t->id);
                    a.bview_.camera().jump_to({t->position.x, t->position.y, 0.0f});
                    a.hold_sim_ = true;  // presentation-only hold: the target stays where the click aims
                    return false;
                }
                if (st->phase == 0 && ++*settle < 10) return false;  // let the camera settle on the target
                if (st->phase == 0) {  // the pixel is fixed for the whole click (the camera may still be moving)
                    const ui::View v = ui::make_view(a.bview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                    float sx = 0, sy = 0;
                    if (!ui::project(v, {t->position.x, t->position.y, t->position.z}, sx, sy)) return false;
                    *px = {static_cast<int32_t>(sx), static_cast<int32_t>(sy)};
                }
                const bool done = click_at(*st, f, px->first, px->second, ui::Button::Right, shift);
                if (done) a.hold_sim_ = false;
                return done;
            },
            600};
}

// Battle: right-click the ground next to the local units (MoveToPosition).
ScriptStep step_move_ground(bool shift = false) {
    auto st = std::make_shared<ClickState>();
    return {shift ? "shift+right-click the ground" : "right-click the ground", [st, shift](App& a, ui::InputFrame& f) {
                if (!a.bview_.active() || a.screen_ != Screen::Battle) return false;
                return click_at(*st, f, static_cast<int32_t>(a.targets_.width / 2), static_cast<int32_t>(a.targets_.height / 3), ui::Button::Right,
                                shift);
            },
            100};
}

// Drag-select box (Playtest 3, opt-in: ALAMO_SMOKE_DRAG_BOX=1, so the default smoke command stream is unchanged): press the
// left button at 30 % / 25 % of the screen, drag to 70 % / 60 % over 10 frames, take `file` with the button still held (the
// box drawn mid-drag), then release (the box selection).
ScriptStep step_drag_box(std::string file) {
    struct State {
        int phase = 0;
        bool drawn = false;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"drag-select box", [st](App& a, ui::InputFrame& f) {
                if (!a.bview_.active() || a.screen_ != Screen::Battle) return false;
                const auto w = static_cast<float>(a.targets_.width), h = static_cast<float>(a.targets_.height);
                const int kMove = 10;
                const float t = static_cast<float>(std::clamp(st->phase, 0, kMove)) / static_cast<float>(kMove);
                f.mouse_x = static_cast<int32_t>(w * (0.30f + 0.40f * t));
                f.mouse_y = static_cast<int32_t>(h * (0.25f + 0.35f * t));
                f.mouse_inside = true;
                const uint8_t bit = ui::button_bit(ui::Button::Left);
                if (st->phase < 0) {  // released last frame: done
                    std::printf("script: drag box drawn while held %d\n", st->drawn ? 1 : 0);
                    if (!st->drawn) a.notes_.push_back("drag-select box not drawn while dragging");
                    return true;
                }
                f.buttons_down = bit;
                if (st->phase == 0) f.buttons_pressed = bit;
                if (st->phase <= kMove) {
                    ++st->phase;
                    return false;
                }
                st->drawn = st->drawn || a.ui_.tactical_hud.stats().drag_box;
                if (!st->shot.run(a, f)) return false;
                f.buttons_down = 0;
                f.buttons_released = bit;
                st->phase = -1;
                return false;
            },
            1400};
}

// R2 t03 (ALAMO_SMOKE_ZOOM_LIGHT=1, campaign): zoom onto the first planet and shoot it with the camera settled (the galactic
// light record at its zoomed-in angles / intensity: GalacticView::use_scene blends with camera().zoom_blend()), then zoom
// back out. Presentation only (camera + selection), no command.
ScriptStep step_zoom_light(std::string file) {
    struct State {
        int phase = 0, wait = 0;
        ScriptStep shot;
    };
    auto st = std::make_shared<State>();
    st->shot = step_shot(std::move(file));
    return {"zoom light", [st](App& a, ui::InputFrame& f) {
                if (a.screen_ != Screen::Galactic) return false;
                if (st->phase == 0) {
                    for (const ui::PickSphere& p : a.gview_.pick_items()) {
                        bool seen = false;  // a planet the local player sees (a fogged one is a flat wireframe globe)
                        for (const render::GalacticPlanetSnap& ps : a.gview_.frame().planets)
                            seen = seen || (static_cast<uint32_t>(ps.id) == p.id && !(ps.flags & render::kGalFogged));
                        if (p.kind == ui::PickKind::Planet && seen) {
                            a.selected_planet_ = p.id;
                            a.gview_.camera().set_look_at({p.center.x, p.center.y, 0.0f});
                            a.gview_.camera().zoom_to_planet(p.center, p.radius);
                            st->phase = 1;
                            break;
                        }
                    }
                    return false;
                }
                if (st->phase == 1) {  // settled zoomed in (zoom state 3)
                    if (a.gview_.camera().zoom_state() == 3 && ++st->wait > 5) st->phase = 2;
                    return false;
                }
                if (st->phase == 2) {
                    if (!st->shot.run(a, f)) return false;
                    std::printf("script: zoomed light blend %.2f (planet %u)\n", static_cast<double>(a.gview_.camera().zoom_blend()), a.selected_planet_);
                    a.gview_.camera().zoom_out();
                    st->phase = 3;
                    return false;
                }
                if (a.gview_.camera().zoom_state() != 0) return false;
                a.selected_planet_ = 0;
                return true;
            }, 3000};
}

// Playtest 3 fleet slots (ALAMO_SMOKE_FLEET_SLOTS=1, campaign): select an own orbiting fleet (shot: every planet's free
// slots as ovals), then drag its map button over a neighbour's slot (shot: the dragged stack, the hovered oval) and
// release it back over its own planet (no order).
ScriptStep step_fleet_slots(std::string sel_file, std::string drag_file) {
    struct State {
        int phase = 0;
        uint32_t fleet = 0;
        PlanetIndex at = kInvalidPlanet, to = kInvalidPlanet;
        ui::Vec2f from{}, dest{};
        ScriptStep shot_sel, shot_drag;
    };
    auto st = std::make_shared<State>();
    st->shot_sel = step_shot(std::move(sel_file));
    st->shot_drag = step_shot(std::move(drag_file));
    return {"fleet slots", [st](App& a, ui::InputFrame& f) {
                if (st->phase < 0) return true;  // released last frame
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                if (st->phase == 0) {
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != a.local_player() || fl.moving || fl.planet == kInvalidPlanet || fl.contents.empty()) continue;
                        for (uint32_t e = g->graph().edge_begin(fl.planet); e < g->graph().edge_end(fl.planet) && st->to == kInvalidPlanet; ++e)
                            if (g->planets()[g->graph().neighbour(e)].is_planet) st->to = g->graph().neighbour(e);
                        if (st->to == kInvalidPlanet) continue;
                        st->fleet = static_cast<uint32_t>(store.id(fl.object));
                        st->at = fl.planet;
                        break;
                    }
                    if (st->fleet == 0) return false;
                    const Vec3 c0 = g->planets()[st->at].position, c1 = g->planets()[st->to].position;
                    a.gview_.camera().set_look_at({0.5f * (c0.x + c1.x), 0.5f * (c0.y + c1.y), 0.0f});
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::GalacticSelect;
                    act.ids = {st->fleet};
                    a.handle_hud_actions({act});
                    st->phase = 1;
                    return false;
                }
                if (st->phase < 4) {  // camera and HUD catch up
                    ++st->phase;
                    return false;
                }
                if (st->phase == 4) {
                    if (!st->shot_sel.run(a, f)) return false;
                    std::printf("script: fleet %u selected, slots shown %d, empty ovals %u\n", st->fleet,
                                a.ui_.galactic_hud.stats().fleet_slots_shown ? 1 : 0, a.ui_.galactic_hud.stats().empty_slots);
                    a.orders_.set_selection({});
                    st->phase = 5;
                    return false;
                }
                const ui::HudCanvas& cv = a.ui_.galactic_hud.canvas();
                if (st->phase == 5) {  // the fleet's map button: the g_planet_fleet hit nearest above its planet
                    const int32_t gpf = a.ui_.components.find("g_planet_fleet");
                    const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                    float px = 0, py = 0, qx = 0, qy = 0;
                    const Vec3 p0 = g->planets()[st->at].position, p1 = g->planets()[st->to].position;
                    if (!ui::project(v, {p0.x, p0.y, p0.z}, px, py) || !ui::project(v, {p1.x, p1.y, p1.z}, qx, qy)) return false;
                    float best = 1e30f;
                    for (const ui::HudHitBox& h : a.ui_.galactic_hud.hit_boxes()) {
                        if (h.component != gpf) continue;
                        const ui::Vec2f c = cv.canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
                        const float d = std::hypot(c.x - px, c.y - py);
                        if (d < best) {
                            best = d;
                            st->from = c;
                        }
                    }
                    if (best > 1e29f) return false;
                    const ui::Vec2f at = cv.pixel_to_canvas(qx, qy);  // the neighbour's middle slot (0, Offset.y 47)
                    st->dest = cv.canvas_to_pixel(at.x, at.y + 47.0f);
                    st->phase = 6;
                }
                const uint8_t bit = ui::button_bit(ui::Button::Left);
                const int kMove = 12;
                f.mouse_inside = true;
                const float t = static_cast<float>(std::clamp(st->phase - 6, 0, kMove)) / static_cast<float>(kMove);
                f.mouse_x = static_cast<int32_t>(st->from.x + (st->dest.x - st->from.x) * t);
                f.mouse_y = static_cast<int32_t>(st->from.y + (st->dest.y - st->from.y) * t);
                f.buttons_down = bit;
                if (st->phase == 6) f.buttons_pressed = bit;
                if (st->phase < 6 + kMove + 2) {
                    ++st->phase;
                    return false;
                }
                if (!st->shot_drag.run(a, f)) return false;
                std::printf("script: fleet drag active %d, over slot %d of planet %u, valid %d, empty ovals %u, highlights %u\n",
                            a.ui_.galactic_hud.drag().active ? 1 : 0, a.ui_.galactic_hud.drag().over_slot,
                            a.ui_.galactic_hud.drag().over_slot_planet, a.ui_.galactic_hud.drag().valid ? 1 : 0,
                            a.ui_.galactic_hud.stats().empty_slots, a.ui_.galactic_hud.stats().slot_highlights);
                f.mouse_x = static_cast<int32_t>(st->from.x);  // back over its own planet: no order
                f.mouse_y = static_cast<int32_t>(st->from.y);
                f.buttons_down = 0;
                f.buttons_released = bit;
                st->phase = -1;
                return false;
            },
            3000};
}

// Battle combat (land and space skirmish): scripted attack orders through the command stream (recorded like any order) -
// every 150 frames the local combat units attack the enemy unit nearest to their centroid; the camera follows them. Done
// 60 frames after the first death (shots fired); the corpse / decal / animation counts are printed. A battle that ends
// first times the step out (the smoke fails): the space skirmish smoke had no combat for a day without failing.
ScriptStep step_combat(bool land) {
    struct State {
        uint32_t n = 0, first_death = 0;
    };
    auto st = std::make_shared<State>();
    return {land ? "land forces engage" : "space forces engage", [st, land](App& a, ui::InputFrame&) {
                const sim::TacticalBattle* b = a.session_ ? a.session_->battle() : nullptr;
                if (!b || !a.bview_.active() || a.screen_ != Screen::Battle) return false;
                // Sim reads only (the battle's movable units, ascending id, store positions): the orders do not depend
                // on the render interpolation.
                const int32_t me = a.local_player();
                const objects::ObjectStore& store = a.session_->host().modes().top_tactical()->primary();
                std::vector<uint32_t> mine;
                double cx = 0, cy = 0;
                for (const objects::ObjectId id : b->movement().unit_ids()) {
                    const objects::ObjectHandle h = store.find(id);
                    if (!store.alive(h) || store.owner(h) != me || !b->combat().has_unit(id)) continue;
                    mine.push_back(static_cast<uint32_t>(id));
                    cx += store.position(h).x;
                    cy += store.position(h).y;
                }
                if (!mine.empty()) {
                    cx /= static_cast<double>(mine.size());
                    cy /= static_cast<double>(mine.size());
                    a.bview_.camera().jump_to({static_cast<float>(cx), static_cast<float>(cy), 0.0f});
                }
                if (st->n++ % 150 == 0 && !mine.empty()) {
                    uint32_t target = 0;
                    double best = 0;
                    for (const objects::ObjectId id : b->movement().unit_ids()) {
                        const objects::ObjectHandle h = store.find(id);
                        if (!store.alive(h) || store.owner(h) < 0 || !b->combat().has_unit(id) || !b->combat().is_enemy(me, store.owner(h))) continue;
                        const double dx = store.position(h).x - cx, dy = store.position(h).y - cy, d = dx * dx + dy * dy;
                        if (target == 0 || d < best) {
                            target = static_cast<uint32_t>(id);
                            best = d;
                        }
                    }
                    if (target != 0) a.submit(commands::ev::attack(target, mine));
                }
                const sim::BattleStats& bs = b->stats();
                if (bs.deaths > 0 && st->first_death == 0) st->first_death = st->n;
                // Sim-only end condition (frame counts, no render state): the run's ticks stay machine independent.
                if (bs.shots == 0 || bs.deaths == 0 || st->n < st->first_death + 60) return false;
                if (land && a.bview_.stats().corpses == 0) a.notes_.push_back("land combat: no corpse drawn yet");
#ifdef ALAMO_SIM_HAVE_ANIM
                // Animation feed: land infantry / vehicles walk, idle and attack (anim plays); every battle's shots flash
                // muzzles. FoC's space capital ships ship no idle / move / attack clips (only _D_DIE_00), so space
                // skirmishes legitimately log 0 anim plays.
                if (const sim::SimAnimation* an = a.session_->animation()) {
                    if (land && bs.anim_plays == 0) a.notes_.push_back("land combat: no animation played");
                    if (an->stats().muzzle_flashes == 0) a.notes_.push_back("combat: no muzzle flash");
                }
#endif
                if (!land && a.bview_.stats().lasers_peak == 0) a.notes_.push_back("space combat: no laser bolt drawn");
                std::printf("script: %s combat - %" PRIu64 " shots, %" PRIu64 " hits, %u deaths, %u corpse draws, %" PRIu64 " scorch decals, %u anim plays, %u peak laser bolts, %u impact / death effects\n",
                            land ? "land" : "space", bs.shots, bs.hits, bs.deaths, a.bview_.stats().corpses, a.bview_.stats().decals, bs.anim_plays,
                            a.bview_.stats().lasers_peak, bs.particles);
                return true;
            },
            6000};
}

// Battle (P4 host): a reinforcement from the pad - the pad's BeginReinforcement action for the first reserve, then a left
// click in the world at the screen centre (the camera is on the local units) = the drop point -> Reinforce in the command
// stream. A drop the cap refuses (QueueFull) is repeated every 90 frames until one deployed or 900 frames passed (the
// battle may end first). Without reserves: a note after 60 frames.
constexpr int kSmokeSkirmishBuys = 14;  // units bought in the space skirmish smoke (more than the space unit cap holds)
constexpr const char* kSmokeSkirmishBuyType = "Corellian_Corvette";
ScriptStep step_reinforce() {
    struct State {
        int phase = 0;
        uint32_t n = 0, next = 0;
        uint64_t rejected0 = 0;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {"reinforcement drop", [st](App& a, ui::InputFrame& f) {
                const sim::TacticalBattle* b = a.session_ ? a.session_->battle() : nullptr;
                if (!b || a.screen_ != Screen::Battle || !a.bview_.active()) {
                    if (st->n > 0) a.notes_.push_back("reinforcement: the battle ended before a drop deployed");
                    return st->n > 0;
                }
                if (st->n++ == 0) st->rejected0 = a.session_->order_stats().rejected;
                const uint64_t refused = a.session_->order_stats().rejected - st->rejected0;
                if (a.session_->order_stats().reinforcements > 0 || (a.reinforce_drops_ > 0 && refused > 0 && st->n >= st->next)) {
                    // Deployed, or answered QueueFull (the cap is full until a unit dies; deployment after a loss is covered
                    // by alamo_game_session_tests).
                    std::printf("script: reinforcement drop answered: %s after %" PRIu64 " drop(s), %" PRIu64 " refused\n",
                                a.session_->order_stats().reinforcements > 0 ? "deployed" : "refused (cap full)", a.reinforce_drops_, refused);
                    return true;
                }
                if (st->n > 900) {
                    a.notes_.push_back("reinforcement: no drop deployed within 900 frames");
                    return true;
                }
                if (st->phase == 0) {
                    const auto pool = b->reserves(a.local_player());
                    if (pool.empty()) {
                        if (st->n > 60 && a.reinforce_drops_ == 0) {
                            a.notes_.push_back("reinforcement: no reserves in this battle");
                            return true;
                        }
                        return false;
                    }
                    if (st->n < st->next) return false;
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::BeginReinforcement;
                    act.unit = static_cast<uint32_t>(pool[0].galactic_id);
                    a.handle_hud_actions({act});
                    st->phase = 1;
                    return false;
                }
                if (click_at(st->click, f, static_cast<int32_t>(a.targets_.width / 2), static_cast<int32_t>(a.targets_.height / 2), ui::Button::Left)) {
                    st->click = {};
                    st->phase = 0;
                    st->next = st->n + 90;
                }
                return false;
            },
            2400};
}

// Battle (QA D11, opt-in: ALAMO_SMOKE_REINFORCE_HOVER=1, so the default smoke command stream is unchanged): the cursor over
// the first reinforcement button r_0000 with the sim held; the shot shows its hover frame over the unit's icon.
ScriptStep step_reinforce_hover(std::string file) {
    auto n = std::make_shared<uint32_t>(0);
    return {"reinforcement button hover", [n, file](App& a, ui::InputFrame& f) {
                if (!a.session_ || a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                const int32_t ci = a.ui_.components.find("r_0000");
                if (ci < 0 || a.ui_.tactical_hud.stats().reinforcements == 0) {
                    if (++*n > 600) {
                        a.notes_.push_back("reinforcement hover: no reinforcement button shown");
                        return true;
                    }
                    return false;
                }
                const ui::ComponentPlacement& p = a.ui_.tactical_hud.layout().placement(static_cast<uint32_t>(ci));
                const auto px = a.ui_.tactical_hud.canvas().canvas_to_pixel(p.center.x, p.center.y);
                f.mouse_x = static_cast<int32_t>(px.x);
                f.mouse_y = static_cast<int32_t>(px.y);
                f.mouse_inside = true;
                a.hold_sim_ = true;
                if (*n < 1000) {  // a few held frames: the button takes the hover before the shot
                    *n = 1000;
                    a.shot_done_ = false;
                    return false;
                }
                if (++*n == 1005 && !a.opt_.png_dir.empty()) a.shot_path_ = a.opt_.png_dir + "/" + file;
                if (*n < 1005 || (!a.opt_.png_dir.empty() && !a.shot_done_)) return false;
                a.hold_sim_ = false;
                return true;
            },
            1200};
}

// Battle: the targeted-ability pick (TargetingController). Begins it as an ability button's HudAction does, then right-clicks
// the middle of the screen: the pick takes the press (the router gives it the click, so the world issues no move / stop
// order) and the right click cancels it. Sends no command, so the smoke CRCs stay put.
ScriptStep step_ability_pick() {
    struct State {
        int phase = 0;
        ClickState click;
        uint64_t routed0 = 0;
    };
    auto st = std::make_shared<State>();
    return {"targeted ability pick", [st](App& a, ui::InputFrame& f) {
                if (!a.session_ || a.screen_ != Screen::Battle || !a.bview_.active()) return false;
                if (st->phase == 0) {
                    a.hold_sim_ = true;  // presentation-only hold: no tick runs during the step (the later steps keep their ticks)
                    ui::HudAction act;
                    act.kind = ui::HudAction::Kind::BeginAbilityTargeting;
                    act.ability_type = 30;  // TRACTOR_BEAM: an object pick
                    act.ids = a.orders_.selection();
                    st->routed0 = a.session_->order_stats().routed;
                    a.handle_hud_actions({act});
                    if (!a.targeting_.active() || a.targeting_.target_kind() != ui::TargetKind::Object) {
                        a.notes_.push_back("ability pick: the HUD action did not start the pick");
                        a.smoke_ok_ = false;
                        a.hold_release_ = true;
                        return true;
                    }
                    st->phase = 1;
                    return false;
                }
                if (!click_at(st->click, f, static_cast<int32_t>(a.targets_.width / 2), static_cast<int32_t>(a.targets_.height / 2), ui::Button::Right))
                    return false;
                a.hold_release_ = true;  // the next step's first frame runs the ticks the held frames did not
                const bool ok = !a.targeting_.active() && a.targeting_.cancelled() >= 1 && a.session_->order_stats().routed == st->routed0;
                std::printf("script: ability pick - %s (cancelled %" PRIu64 ", orders routed +%" PRIu64 ")\n", ok ? "right click cancelled it, no order" : "FAILED",
                            a.targeting_.cancelled(), a.session_->order_stats().routed - st->routed0);
                if (!ok) {
                    a.notes_.push_back("ability pick: the right click did not cancel the pick cleanly");
                    a.smoke_ok_ = false;
                }
                return true;
            },
            600};
}

// Galactic: pick (human combat fleet, hostile planet with defenders in orbit or a starbase) by BFS hops, select the fleet,
// then right-click the planet.
ScriptStep step_galactic_attack() {
    struct State {
        int phase = 0;
        uint32_t fleet = 0, planet = 0;
        PlanetIndex fleet_at = kInvalidPlanet, target = kInvalidPlanet;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {"galactic attack order", [st](App& a, ui::InputFrame& f) {
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                const int32_t me = a.local_player();
                if (st->phase == 0) {
                    uint32_t best_hops = 0xFFFFFFFFu;
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != me || fl.moving || fl.planet == kInvalidPlanet) continue;
                        bool combat = false;
                        for (const objects::ObjectHandle m : fl.contents)
                            combat = combat || (store.alive(m) && g->rules().types[store.type(m)].ai_combat_power > 0.0f &&
                                                g->rules().types[store.type(m)].hyperspace);
                        if (!combat) continue;
                        std::vector<uint32_t> hops(g->planets().size(), 0xFFFFFFFFu);
                        std::deque<PlanetIndex> q{fl.planet};
                        hops[fl.planet] = 0;
                        while (!q.empty()) {
                            const PlanetIndex at = q.front();
                            q.pop_front();
                            const galactic::PlanetState& p = g->planets()[at];
                            const bool defended = p.starbase_level > 0 || !g->fleets_at(at, p.owner).empty();
                            if (at != fl.planet && p.is_planet && p.owner >= 0 && g->is_hostile(p.owner, me) && defended && hops[at] < best_hops) {
                                best_hops = hops[at];
                                st->fleet = static_cast<uint32_t>(store.id(fl.object));
                                st->fleet_at = fl.planet;
                                st->target = at;
                                st->planet = static_cast<uint32_t>(store.id(p.object));
                            }
                            for (uint32_t e = g->graph().edge_begin(at); e < g->graph().edge_end(at); ++e) {
                                const PlanetIndex nb = g->graph().neighbour(e);
                                if (nb < hops.size() && hops[nb] == 0xFFFFFFFFu) {
                                    hops[nb] = hops[at] + 1;
                                    q.push_back(nb);
                                }
                            }
                        }
                    }
                    if (st->target == kInvalidPlanet) return false;  // nothing to attack yet: wait (the AI moves too)
                    std::printf("script: fleet %u at %s attacks %s (%u hops)\n", st->fleet, g->map().planets.name[st->fleet_at].c_str(),
                                g->map().planets.name[st->target].c_str(), best_hops);
                    const Vec3 c = g->planets()[st->fleet_at].position;
                    a.gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    st->phase = 1;
                    return false;
                }
                const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                if (st->phase == 1 || st->phase == 3) {  // let the camera / frame catch up one frame
                    ++st->phase;
                    if (st->phase == 4) {
                        const Vec3 c = g->planets()[st->target].position;
                        a.gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    }
                    return false;
                }
                if (st->phase == 2) {  // select the fleet: an orbiting fleet is picked through its HUD planet fleet button
                    ui::HudAction act;  // (GalacticSelect {fleet}); the map picks transit fleets only
                    act.kind = ui::HudAction::Kind::GalacticSelect;
                    act.ids = {st->fleet};
                    a.handle_hud_actions({act});
                    st->phase = 3;
                    return false;
                }
                if (st->phase == 4 || st->phase == 5) {  // two frames for the camera move, then right-click the planet
                    ++st->phase;
                    return false;
                }
                const Vec3 p = g->planets()[st->target].position;
                float sx = 0, sy = 0;
                if (!ui::project(v, {p.x, p.y, p.z}, sx, sy)) return false;
                return click_at(st->click, f, static_cast<int32_t>(sx), static_cast<int32_t>(sy), ui::Button::Right);
            },
            30000};
}

// Galactic: a plain fleet hop by input, all the way to arrival (playtest "units can't move planet to planet"). An own
// idle fleet and an adjacent planet that is not hostile (no battle); `drag`: left-drag the fleet onto the planet, else
// left-click the fleet and right-click the planet. Fails the run when the order is refused or the fleet never arrives.
ScriptStep step_galactic_move(bool drag) {
    struct State {
        int phase = 0;
        uint32_t fleet = 0, planet = 0;
        PlanetIndex from = kInvalidPlanet, to = kInvalidPlanet;
        objects::ObjectHandle member;  // the fleet may merge into another one on arrival: followed by its first member
        uint64_t refused0 = 0;
        int drag_frame = 0;
        ClickState click;
    };
    auto st = std::make_shared<State>();
    return {drag ? "galactic move by drag" : "galactic move by click", [st, drag](App& a, ui::InputFrame& f) {
                const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
                if (!g || a.screen_ != Screen::Galactic) return false;
                const objects::ObjectStore& store = g->store();
                const int32_t me = a.local_player();
                if (st->phase == 0) {
                    for (const galactic::FleetState& fl : g->fleets()) {
                        if (!fl.alive || fl.owner != me || fl.moving || fl.planet == kInvalidPlanet || fl.contents.empty()) continue;
                        bool hyper = true;
                        for (const objects::ObjectHandle m : fl.contents) hyper = hyper && store.alive(m) && g->rules().types[store.type(m)].hyperspace;
                        if (!hyper) continue;
                        for (uint32_t e = g->graph().edge_begin(fl.planet); e < g->graph().edge_end(fl.planet); ++e) {
                            const PlanetIndex nb = g->graph().neighbour(e);
                            const galactic::PlanetState& p = g->planets()[nb];
                            if (!p.is_planet || (p.owner >= 0 && g->is_hostile(p.owner, me))) continue;
                            if (!g->planet_revealed_raw(nb)) continue;  // GRC 1.1: the hop gate (revealed after a pre-step)
                            bool enemies = false;
                            for (const galactic::FleetState& o : g->fleets())
                                enemies = enemies || (o.alive && o.owner >= 0 && o.owner != me && g->is_hostile(o.owner, me) && (o.planet == nb || o.to == nb));
                            if (enemies) continue;
                            st->fleet = static_cast<uint32_t>(store.id(fl.object));
                            st->member = fl.contents.front();
                            st->from = fl.planet;
                            st->to = nb;
                            st->planet = static_cast<uint32_t>(store.id(p.object));
                            break;
                        }
                        if (st->to != kInvalidPlanet) break;
                    }
                    if (st->to == kInvalidPlanet) return false;  // none idle yet: wait
                    std::printf("script: fleet %u moves %s -> %s by %s\n", st->fleet, g->map().planets.name[st->from].c_str(),
                                g->map().planets.name[st->to].c_str(), drag ? "drag" : "click");
                    // Both planets on screen: the camera looks at their midpoint.
                    const Vec3 c0 = g->planets()[st->from].position, c1 = g->planets()[st->to].position;
                    a.gview_.camera().set_look_at({0.5f * (c0.x + c1.x), 0.5f * (c0.y + c1.y), 0.0f});
                    a.orders_.set_selection({});
                    st->refused0 = a.refused_;
                    st->phase = 1;
                    return false;
                }
                if (st->phase < 3) {  // camera and frame catch up
                    ++st->phase;
                    return false;
                }
                const ui::View v = ui::make_view(a.gview_.camera_params(a.targets_.width, a.targets_.height), a.targets_.width, a.targets_.height);
                float fx = 0, fy = 0, px = 0, py = 0;
                const render::GalacticFleetSnap* fs = nullptr;
                for (const render::GalacticFleetSnap& x : a.gview_.frame().fleets)
                    if (static_cast<uint32_t>(x.id) == st->fleet) fs = &x;
                if (st->phase == 3 || st->phase == 4) {
                    // The orbiting fleet has no map snapshot (only fleets in transit are drawn): only the target planet
                    // needs a screen position.
                    (void)fs;
                    (void)fx;
                    (void)fy;
                    const Vec3 pp = g->planets()[st->to].position;
                    if (!ui::project(v, {pp.x, pp.y, pp.z}, px, py)) return false;
                    // GALAXY_MAP_PRESENTATION §2: an orbiting fleet is no longer a pick target (only fleets in transit are
                    // drawn); it is selected through its HUD planet fleet button = HudAction GalacticSelect {fleet}.
                    // The smoke issues the button's action directly (no screen position for the button), so the drag
                    // variant ends with the same right-click order as the click variant.
                    if (st->phase == 3) {
                        ui::HudAction act;
                        act.kind = ui::HudAction::Kind::GalacticSelect;
                        act.ids = {st->fleet};
                        a.handle_hud_actions({act});
                        st->phase = 4;
                        return false;
                    }
                    if (click_at(st->click, f, static_cast<int32_t>(px), static_cast<int32_t>(py), ui::Button::Right)) st->phase = 5;
                    return false;
                }
                // Order sent: refused -> fail; else wait for the fleet to leave and to orbit the target.
                if (a.refused_ != st->refused0) {
                    std::fprintf(stderr, "script: galactic move refused (see the submit line)\n");
                    a.notes_.push_back("galactic move: order refused");
                    a.smoke_ok_ = false;
                    return true;
                }
                if (!store.alive(st->member)) {
                    std::fprintf(stderr, "script: the moving fleet's unit died\n");
                    a.script_failed_ = true;
                    return true;
                }
                for (const galactic::FleetState& fl : g->fleets())
                    if (fl.alive && std::find(fl.contents.begin(), fl.contents.end(), st->member) != fl.contents.end()) {
                        if (st->phase == 5 && fl.moving) {
                            std::printf("script: fleet %u left %s\n", st->fleet, g->map().planets.name[st->from].c_str());
                            st->phase = 6;
                        }
                        if (st->phase == 6 && !fl.moving && fl.planet == st->to) {
                            std::printf("script: fleet %u arrived at %s\n", st->fleet, g->map().planets.name[st->to].c_str());
                            return true;
                        }
                    }
                return false;
            },
            6000};
}

void App::make_script() {
    auto& s = script_;
    // Intro movies (when FFmpeg's Bink decoder is present): one frame shown, PNG, then Esc skips each one.
    s.push_back({"intro movies skipped", [](App& a, ui::InputFrame& f) {
                     static int n = 0;
                     if (a.screen_ != Screen::Movie) return true;
                     if (++n % 20 == 0) {
                         f.keys_down.set(ui::vk::kEscape);
                         f.keys_pressed.set(ui::vk::kEscape);
                     }
                     if (n == 10 && a.full_movie_.has_frame() && !a.opt_.png_dir.empty()) a.shot_path_ = a.opt_.png_dir + "/game_intro_movie.png";
                     return false;
                 },
                 1200});
    s.push_back(step_wait("menu shown", [](App& a) { return a.ui_.gui.top() != nullptr && a.frames_ > 2; }, 600));
    s.push_back(step_shot("game_main_menu.png"));
    if (opt_.smoke == "skirmish") {
        // OI host: the attract demo behind the main menu (MAIN_MENU_BACKGROUND.md; started here as the menu-open path would,
        // scripted runs never start it on their own): its battle world is drawn under the menu (world draws > 0, no
        // tactical HUD) and keeps ticking under the menu and its sub-dialogs (input never stops it); the skirmish's start
        // ends it. Its own session: the skirmish's session, CRC log and recording start afresh afterwards.
        s.push_back(step_wait("attract demo starts", [](App& a) {
            if (a.attract_.menu_opened(a.now_s_, true) == AttractController::Request::None || !a.start_attract_map()) {
                a.notes_.push_back("attract demo did not start");
                a.smoke_ok_ = false;
                a.stop_attract();
            }
            return true;
        }));
        auto tick0 = std::make_shared<uint64_t>(0);
        s.push_back(step_wait("attract demo drawn under the menu", [tick0](App& a) {
            if (!a.demo_session()) return true;  // failed above
            const sim::TacticalBattle* b = a.session_->battle();
            const bool drawn = a.screen_ == Screen::Menu && a.bview_.active() && b && a.draw_stats_.draws > 0 &&
                               a.ui_.gui.find("IDD_MAIN_MENU_DIALOG") && a.ui_.tactical_hud.stats().quads == 0;
            if (drawn) {
                *tick0 = b->frame();
                std::printf("script: attract demo - %u world draws under the main menu, battle frame %u\n", a.draw_stats_.draws, b->frame());
            }
            return drawn;
        }, 3000));
        s.push_back(step_shot("game_attract_demo.png"));
        // Playtest 3 t14: the demo's hulls must point along their travel direction (models face -Y, facing = the nose; the
        // same convention as the tactical battle): positions at two ticks 120 apart, the later facing against the displacement.
        {
            struct Orient {
                uint64_t t0 = 0;
                std::map<objects::ObjectId, std::array<float, 2>> pos;
            };
            auto st = std::make_shared<Orient>();
            s.push_back(step_wait("attract hull heading vs travel", [st](App& a) {
                const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                if (!b) return true;  // the demo did not start (noted above)
                const std::vector<render::SnapshotObject>& objs = a.bview_.frame().objects;
                if (st->pos.empty()) {
                    st->t0 = b->frame();
                    for (const render::SnapshotObject& o : objs)
                        if (o.owner >= 0 && b->combat().has_unit(o.id)) st->pos[o.id] = {o.position.x, o.position.y};
                    return false;
                }
                if (b->frame() < st->t0 + 120) return false;
                uint32_t moving = 0, forward = 0, sideways = 0, backward = 0;
                double sum_cos = 0.0;
                for (const render::SnapshotObject& o : objs) {
                    const auto it = st->pos.find(o.id);
                    if (it == st->pos.end()) continue;
                    const float dx = o.position.x - it->second[0], dy = o.position.y - it->second[1];
                    const float dl = std::sqrt(dx * dx + dy * dy), fl = std::sqrt(o.facing.x * o.facing.x + o.facing.y * o.facing.y);
                    if (dl < 5.0f || fl < 0.1f) continue;
                    const float c = (dx * o.facing.x + dy * o.facing.y) / (dl * fl);
                    ++moving;
                    sum_cos += static_cast<double>(c);
                    if (c > 0.7f) ++forward;
                    else if (c < -0.7f) ++backward;
                    else if (std::fabs(c) < 0.5f) ++sideways;
                }
                std::printf("script: attract orientation - %u moving hulls over 120 ticks: %u nose-first (cos > 0.7), %u backwards, %u sideways, mean cos %.3f\n",
                            moving, forward, backward, sideways, moving ? sum_cos / static_cast<double>(moving) : 0.0);
                if (moving > 0 && forward * 2 < moving) {  // most moving hulls must fly nose-first
                    a.notes_.push_back("attract hulls are not oriented along their travel direction");
                    a.smoke_ok_ = false;
                }
                return true;
            }, 3000));
            s.push_back(step_shot("game_attract_demo_later.png"));
        }
        // Playtest 3 t14: the demo camera's shots. Four cuts (a forced new shot every 45 ticks, each on the next combat unit),
        // a PNG per shot; at the end the followed hull must not have been seen nose-on / tail-on (within 30 degrees of the
        // view direction) for more than a quarter of the frames (the stand-in camera keeps a side view, attract_mode.hpp).
        {
            auto shot_tick = std::make_shared<uint64_t>(0);
            for (int k = 0; k < 4; ++k) {
                s.push_back(step_wait("attract camera cut", [shot_tick, k](App& a) {
                    const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                    if (!b) return true;
                    if (k == 0) a.demo_stats_ = {};
                    if (k == 0 || b->frame() >= *shot_tick + 45) {
                        *shot_tick = b->frame();
                        a.demo_shot_t0_ = -1.0;  // the next demo_camera() frame cuts to the next unit
                        return true;
                    }
                    return false;
                }, 1200));
                s.push_back(step_wait("attract camera shot runs", [shot_tick](App& a) {
                    const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
                    return !b || b->frame() >= *shot_tick + 44;
                }, 1200));
                s.push_back(step_shot("game_attract_cam_" + std::to_string(k) + ".png"));
            }
            s.push_back(step_wait("attract camera nose-on check", [](App& a) {
                if (!a.demo_session()) return true;
                const auto& st = a.demo_stats_;
                std::printf("script: attract camera (%s) - %u frames, followed hull nose-on/tail-on in %u (longest run %u frames), nearby hulls %u / %u nose-on/tail-on\n",
                            a.demo_side_view_ ? "side view" : "first stand-in", st.frames, st.target_nose_on, st.longest_run, st.hulls_nose_on, st.hulls);
                if (st.frames == 0 || st.target_nose_on * 4 > st.frames) {
                    a.notes_.push_back("attract camera shows the followed hull nose-on / tail-on for a long stretch");
                    a.smoke_ok_ = false;
                }
                return true;
            }));
        }
        s.push_back(step_wait("attract demo ticks under the menu", [tick0](App& a) {
            const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
            return !b || b->frame() >= *tick0 + 30;
        }, 600));
        s.push_back(step_click_control("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
        s.push_back(step_wait("attract demo runs under the single-player dialog", [tick0](App& a) {
            const sim::TacticalBattle* b = a.demo_session() ? a.session_->battle() : nullptr;
            if (!b) {
                a.notes_.push_back("attract demo stopped by the menu input");
                a.smoke_ok_ = false;
                return true;
            }
            const bool ok = a.attract_.running() && a.bview_.active() && a.draw_stats_.draws > 0 && b->frame() >= *tick0 + 60 &&
                            a.ui_.gui.find("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG") != nullptr;
            if (ok) std::printf("script: attract demo - still running under the single-player dialog, battle frame %u\n", b->frame());
            return ok;
        }, 600));
    } else {
        s.push_back(step_click_control("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
    }
    auto battle_steps = [&s](const std::string& prefix) {
        s.push_back(step_wait("load screen ready", [](App& a) { return a.screen_ == Screen::BattleLoad && a.ui_.load.ready(); }, 6000));
        s.push_back(step_shot(prefix + "_load.png"));
        s.push_back({"begin", [](App& a, ui::InputFrame& f) {
                         static ClickState c;  // one Begin per run of this step
                         if (a.screen_ != Screen::BattleLoad) return true;
                         ui::GuiDialog* d = a.ui_.gui.find(a.ui_.load.dialog());
                         const int32_t i = d ? d->find("IDC_BEGIN_BUTTON") : -1;
                         if (i < 0) return false;
                         const ui::GuiRect r = d->control_rect(static_cast<size_t>(i));
                         const ui::Vec2f p = a.ui_.gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
                         if (click_at(c, f, static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), ui::Button::Left)) c = {};
                         return false;
                     },
                     600});
        s.push_back(step_ticks("battle settles", 30));
        s.push_back(step_select_all());
        s.push_back(step_ticks("selection drained", 3));
        add_control_group_steps(s);
        s.push_back(step_encyclopedia("encyclopedia over a unit"));
        s.push_back(step_shot(prefix + "_encyclopedia.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_unit_tooltip(prefix + "_screen_tooltip.png"));
        s.push_back(step_attack_enemy());
        s.push_back(step_wait("attack routed into the battle", [](App& a) { return a.session_ && a.session_->order_stats().attacks >= 1; }, 600));
        s.push_back(step_ticks("battle runs", 60));
        s.push_back(step_wait("camera on the local units", [](App& a) {
            double x = 0, y = 0;
            uint32_t n = 0;
            for (const render::SnapshotObject& o : a.bview_.frame().objects)
                if (o.owner == a.local_player() && a.session_ && a.session_->battle() && a.session_->battle()->movement().has_unit(o.id)) {
                    x += o.position.x;
                    y += o.position.y;
                    ++n;
                }
            if (n) a.bview_.camera().jump_to({static_cast<float>(x / n), static_cast<float>(y / n), 0.0f});
            return true;
        }));
        s.push_back(step_shot(prefix + "_battle.png"));
        if (env_or_empty("ALAMO_SMOKE_DRAG_BOX") == "1") s.push_back(step_drag_box(prefix + "_drag_box.png"));
        s.push_back(step_move_ground());
        s.push_back(step_wait("move routed", [](App& a) { return a.session_ && a.session_->order_stats().moves >= 1; }, 600));
        // Queueable actions: shift+right-click an enemy (queued attack), then shift+right-click the ground (queued move);
        // the HUD shows the selected units' queue markers.
        s.push_back(step_attack_enemy(true));
        s.push_back(step_move_ground(true));
        s.push_back(step_wait("queued orders routed", [](App& a) {
            return a.session_ && a.session_->order_stats().attacks >= 2 && a.session_->order_stats().moves >= 2;
        }, 600));
        s.push_back(step_wait("queue markers shown", [](App& a) {
            if (a.queues_.empty()) return false;
            // Movement path lines of the moving selection (HudWaypointLine, same frame: no extra step / tick).
            const ui::TacticalHudStats& hs = a.ui_.tactical_hud.stats();
            std::printf("script: path lines - %zu units, %u waypoint dots, %u ability buttons\n", a.waypoints_.size(), hs.waypoint_dots,
                        hs.ability_buttons);
            if (a.waypoints_.empty()) a.notes_.push_back("no movement path line for the moving selection");
            return true;
        }, 600));
        s.push_back(step_shot(prefix + "_queued.png"));
        if (env_or_empty("ALAMO_SMOKE_NO_ABILITY_PICK") != "1") s.push_back(step_ability_pick());  // (=1: A/B check, the step moves no CRC)
        if (prefix == "game_skirmish") {  // P4 host: the bought fleet overflows the cap - a reserve dropped from the pad
            if (env_or_empty("ALAMO_SMOKE_REINFORCE_HOVER") == "1") s.push_back(step_reinforce_hover(prefix + "_reinforce_hover.png"));
            s.push_back(step_reinforce());
            s.push_back(step_shot(prefix + "_reinforced.png"));
        }
        add_pause_steps(s);
        if (prefix == "game_land" || prefix == "game_skirmish") {  // combat: the forces engage (shots, deaths, animation, decals)
            s.push_back(step_combat(prefix == "game_land"));
            s.push_back(step_shot(prefix + "_combat.png"));
        }
        // Battle end (time limit): the battle-end screen, then Continue.
        s.push_back(step_wait("battle-end screen", [](App& a) { return a.battle_end_open_ && a.ui_.gui.find(ui::dlg::kBattleEnd); }, 20000));
        s.push_back(step_shot(prefix + "_battle_end.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_victory_screen(prefix + "_screen_victory.png"));
        s.push_back(step_click_control(ui::dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"));
    };
    // Skirmish buy-units screen: buy the first row, sell everything again (the default fleet stays), Start game.
    // P4 host: the space skirmish keeps a bought fleet instead, larger than the space unit cap (reinforcement pool).
    auto buy_steps = [&s](const std::string& prefix) {
        // Row 0; the space skirmish buys corvettes (kSmokeSkirmishBuyType, a cap-counted combat ship) when the list has them.
        const bool space_buy = prefix == "game_skirmish";
        s.push_back(step_select_row(ui::dlg::kBuyUnits, "IDC_SKIRMISH_BUY_FORCES_LISTBOX", [space_buy](App& a) {
            for (size_t i = 0; space_buy && i < a.buy_.units.size(); ++i)
                if (a.buy_.units[i].type == kSmokeSkirmishBuyType) return i;
            return size_t{0};
        }));
        s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_BUY_BUTTON"));
        s.push_back(step_wait("unit bought", [](App& a) { return !a.bought_.empty(); }, 120));
        s.push_back(step_shot(prefix + "_buy_units.png"));
        if (prefix == "game_skirmish") {
            for (int i = 0; i < kSmokeSkirmishBuys - 1; ++i) s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_BUY_BUTTON"));
        } else {
            s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_SELL_ALL_BUTTON"));
        }
        s.push_back(step_click_control(ui::dlg::kBuyUnits, "IDC_NEXT_BUTTON"));
    };
    if (opt_.smoke == "land") {  // land skirmish: the first land map of the list (rows after the space maps)
        s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_SKIRMISH_GAMES_BUTTON"));
        s.push_back(step_select_row("IDD_SKIRMISH_STAGING_DIALOG", "IDC_STAGING_MAP_LIST", [](App& a) { return a.space_map_count_; }));
        s.push_back(step_shot("game_land_staging.png"));
        s.push_back(step_click_control("IDD_SKIRMISH_STAGING_DIALOG", "IDC_SKIRMISH_START_GAME_BUTTON"));
        buy_steps("game_land");
        battle_steps("game_land");
        s.push_back(step_wait("land skirmish over", [](App& a) { return a.session_ && a.skirmish_ended_ && a.screen_ == Screen::Menu; }, 20000));
    } else if (opt_.smoke == "skirmish") {
        s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_SKIRMISH_GAMES_BUTTON"));
        s.push_back(step_shot("game_skirmish_staging.png"));
        s.push_back(step_click_control("IDD_SKIRMISH_STAGING_DIALOG", "IDC_SKIRMISH_START_GAME_BUTTON"));
        buy_steps("game_skirmish");
        battle_steps("game_skirmish");
        s.push_back(step_wait("skirmish over", [](App& a) { return a.session_ && a.skirmish_ended_ && a.screen_ == Screen::Menu; }, 20000));
    } else {
        s.push_back(step_click_control("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_CUSTOM_CAMPAIGN_BUTTON"));
        s.push_back(step_shot("game_campaign_select.png"));
        s.push_back(step_click_control("IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG", "IDC_PLAY_BUTTON"));
        s.push_back(step_wait("galactic map", [](App& a) { return a.screen_ == Screen::Galactic; }, 600));
        s.push_back(step_ticks("galaxy runs", 30));
        // GALACTIC_DIALOGS §2: no dialog opens at campaign start; the opening Story_Dialog event adds its chapter to the
        // mission holocron (b_story_arc_g flashes). Clicking the holocron opens the story summary, which holds the ticks.
        {
            auto n = std::make_shared<int>(0);
            s.push_back({"holocron updated", [n](App& a, ui::InputFrame&) { return a.holocron_updates_ > 0 || ++*n > 300; }, 400});
        }
        s.push_back(step_wait("holocron button opens the story summary", [](App& a) {
            if (a.holocron_chapters_.empty()) return true;  // no story chapter in this campaign
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::Button;
            act.name = "b_story_arc_g";
            a.handle_hud_actions({act});
            return a.story_summary_open_ && a.ui_.gui.find(ui::dlg::kStorySummary) != nullptr;
        }, 10));
        s.push_back(step_shot("game_campaign_story.png"));
        s.push_back(step_click_control(ui::dlg::kStorySummary, "IDC_SUMMARY_DIALOG_CLOSE_BUTTON"));
        s.push_back(step_wait("story summary closed", [](App& a) { return !a.story_summary_open_; }, 300));
        s.push_back(step_shot("game_campaign_galactic.png"));
        if (env_or_empty("ALAMO_SMOKE_FLEET_SLOTS") == "1")
            s.push_back(step_fleet_slots("game_campaign_fleet_slots.png", "game_campaign_fleet_slot_drag.png"));
        if (env_or_empty("ALAMO_SMOKE_ZOOM_LIGHT") == "1") s.push_back(step_zoom_light("game_campaign_zoom_light.png"));
        if (env_or_empty("ALAMO_SMOKE_TRANSIT") == "1") {  // t32: a shot with any fleet in flight (opt-in, gives up after 3000 frames)
            auto n = std::make_shared<int>(0);
            s.push_back({"a fleet in transit", [n](App& a, ui::InputFrame&) {
                             for (const render::GalacticFleetSnap& fl : a.gview_.frame().fleets)
                                 if (fl.flags & render::kGalFleetMoving) {
                                     a.gview_.camera().set_look_at({fl.position.x, fl.position.y, 0.0f});
                                     return true;
                                 }
                             return ++*n > 3000;
                         }, 3100});
            s.push_back(step_shot("game_campaign_transit.png"));
        }
        add_pause_steps(s);  // galactic pause / resume
        s.push_back(step_encyclopedia("encyclopedia over a planet"));
        s.push_back(step_shot("game_campaign_encyclopedia.png"));
        // Plain fleet hops by input, to arrival (click, then drag).
        s.push_back(step_galactic_move(false));
        s.push_back(step_galactic_move(true));
        s.push_back(step_galactic_attack());
        // The pending-battle choice (message box): Fight.
        s.push_back(step_wait("battle prompt shown", [](App& a) { return a.choice_open_; }, 6000));
        s.push_back(step_shot("game_campaign_battle_choice.png"));
        s.push_back(step_wait("Begin", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::BattleBegin;
            a.handle_hud_actions({act});
            return !a.choice_open_;
        }, 10));
        battle_steps("game_campaign");
        s.push_back(step_wait("back on the galactic map", [](App& a) { return a.screen_ == Screen::Galactic && !a.bview_.active() && !a.battle_end_open_; }, 20000));
        // Shot at once: the AI may start the next conflict within a few ticks (the shot holds the sim).
        s.push_back(step_shot("game_campaign_galactic_after.png"));
        // GALACTIC_DIALOGS §3.1: a second attack answered Auto-Resolve - resolved at once (no progress dialog), the red
        // result line, then the battle-end screen.
        s.push_back(step_galactic_attack());
        s.push_back(step_wait("battle prompt shown again", [](App& a) { return a.choice_open_; }, 6000));
        s.push_back(step_wait("Auto-Resolve", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::BattleAutoResolve;
            a.handle_hud_actions({act});
            return !a.choice_open_;
        }, 10));
        s.push_back(step_wait("auto-resolve battle-end screen", [](App& a) { return a.auto_results_ > 0 && a.battle_end_open_ && a.ui_.gui.find(ui::dlg::kBattleEnd) && a.screen_text_.empty(); }, 600));
        s.push_back(step_shot("game_campaign_auto_result.png"));
        s.push_back(step_click_control(ui::dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"));
        // The galaxy stands still from here (presentation-only hold: no AI conflict interrupts the screen checks).
        s.push_back(step_wait("hold the galaxy", [](App& a) { return a.script_hold_ = true; }, 10));
        // In-game options (MAIN_LOOP.md §1.5): the HUD's options button opens the game menu, which pauses the session; the
        // options and their video page keep it paused (the video page lists the display modes); Esc x3 closes them all and
        // the session runs again. The galaxy is held already (no tick moves).
        s.push_back(step_wait("options button opens the game menu", [](App& a) {
            ui::HudAction act;
            act.kind = ui::HudAction::Kind::Button;
            act.name = "b_option_g";
            a.handle_hud_actions({act});
            return a.paused_ && a.ui_.front.pause_menu_open();
        }, 10));
        s.push_back(step_click_control("IDD_GAME_OPTIONS_DIALOG", "IDC_INGAME_OPTIONS"));
        s.push_back(step_click_control("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_IN_GAME_OPTIONS_VIDEO"));
        s.push_back(step_wait("video options list the resolutions", [](App& a) {
            const ui::GuiDialog* d = a.ui_.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
            const ui::GuiControl* l = d ? d->get("IDC_RESOLUTION_LIST") : nullptr;
            if (!d || !a.paused_) return false;
            const size_t n = l ? l->items.size() : 0;
            std::printf("script: video options - %zu resolutions (%zu display modes), selected %d, session paused\n", n, a.display_modes_.size(),
                        l ? l->selected : -1);
            if (n != a.display_modes_.size()) {
                a.notes_.push_back("video options: resolution list not filled");
                a.smoke_ok_ = false;
            }
            return true;
        }, 120));
        s.push_back(step_shot("game_campaign_video_options.png"));
        if (env_or_empty("ALAMO_SMOKE_SCREENS") == "1") s.push_back(step_shot("game_campaign_screen_video.png"));
        s.push_back(step_key("close the video options", ui::vk::kEscape));
        s.push_back(step_key("close the options", ui::vk::kEscape));
        s.push_back(step_key("close the game menu", ui::vk::kEscape));
        s.push_back(step_wait("game menu closed, session unpaused", [](App& a) { return !a.menu_dialog_open() && !a.paused_; }, 120));
        // P4 host: the black-market panel with the local player's real offers (bm_offers) at a planet of another playable
        // faction - a presentation-only preview: the scripted campaign has no natural black-market dialog (that needs an
        // Underworld hero at a Corruption_Black_Market planet), and the preview sends no command.
        s.push_back(step_wait("black market panel", [](App& a) {
            const galactic::GalacticSim* g = a.session_ ? a.session_->galaxy() : nullptr;
            if (!g) return true;
            if (!a.panel_preview_) {
                for (size_t i = 0; i < g->planets().size() && !a.preview_planet_; ++i) {
                    const galactic::PlanetState& p = g->planets()[i];
                    if (!p.is_planet || p.owner == a.local_player() || p.owner < 0 || static_cast<size_t>(p.owner) >= g->players().size()) continue;
                    const uint32_t f = g->players()[static_cast<size_t>(p.owner)].faction;
                    if (f < g->rules().factions.size() && g->rules().factions[f].is_playable) a.preview_planet_ = static_cast<uint32_t>(g->store().id(p.object));
                }
                a.panel_preview_ = galactic::kSpecialBlackMarket;
            }
            return a.market_.open() || !a.preview_planet_;  // no such planet: nothing to show
        }, 120));
        s.push_back(step_shot("game_campaign_black_market.png"));
        s.push_back(step_wait("black market closed", [](App& a) {
            a.panel_preview_ = 0;
            return true;
        }, 10));
        // Holocron (hotkey): droid log, then the tech tree and the production summary pages; Esc closes.
        s.push_back(step_hotkey("advisor key", ui::hk::kDroidAdvisor));
        s.push_back(step_wait("droid log shown", [](App& a) { return a.info_.info_dialog() == ui::dlg::kDroidLog; }, 300));
        s.push_back(step_shot("game_campaign_droid_log.png"));
        s.push_back(step_wait("tech tree page", [](App& a) {
            a.open_holocron(ui::InfoPage::TechTree);
            return !a.info_.info_dialog().empty() && a.info_.info_page() == ui::InfoPage::TechTree;
        }, 300));
        s.push_back(step_shot("game_campaign_tech_tree.png"));
        s.push_back(step_wait("production summary page", [](App& a) {
            a.open_holocron(ui::InfoPage::SummaryProduction);
            return a.info_.info_dialog() == ui::dlg::kSummaryProduction;
        }, 300));
        s.push_back(step_shot("game_campaign_summary.png"));
        s.push_back(step_key("close the holocron", ui::vk::kEscape));
        s.push_back(step_wait("holocron closed", [](App& a) { return a.info_.info_dialog().empty(); }, 300));
        // Story summary (b_story_arc_g opens it; [smoke] called directly): objectives + recap of the local plot set.
        s.push_back({"story summary", [n = std::make_shared<int>(0)](App& a, ui::InputFrame&) {
                         if (*n == 1) a.open_story_summary();
                         return ++*n > 3;
                     },
                     100});
        s.push_back(step_wait("story summary shown", [](App& a) { return a.ui_.gui.find(ui::dlg::kStorySummary) != nullptr; }, 300));
        s.push_back(step_shot("game_campaign_story_summary.png"));
        s.push_back(step_key("close the story summary", ui::vk::kEscape));
        s.push_back(step_wait("story summary closed", [](App& a) { return a.ui_.gui.find(ui::dlg::kStorySummary) == nullptr; }, 300));
        // Campaign end: [smoke] forced (the galaxy is not conquered in a smoke run) - the campaign score, then Quit.
        s.push_back(step_wait("campaign score", [](App& a) {
            if (!a.campaign_over_) a.show_campaign_end(true);
            return a.ui_.gui.find(ui::dlg::kCampaignScore) != nullptr;
        }, 300));
        s.push_back(step_shot("game_campaign_score.png"));
        s.push_back(step_click_control(ui::dlg::kCampaignScore, "IDC_QUIT_BUTTON"));
        s.push_back(step_wait("main menu after the campaign", [](App& a) { return a.screen_ == Screen::Menu; }, 300));
    }
}
// ------------------------------------------------------------------------------------------------ run
int App::run() {
    if (!init()) return 1;
    while (screen_ != Screen::Quit && win_.pump()) {
        frame();
        update_display_rate(false);
        limiter_.wait();  // [port: Gabriel] frame rate cap (live play only)
        if (opt_.frames >= 0 && static_cast<long>(frames_) >= opt_.frames) break;
        if (!script_.empty() && script_step_ >= script_.size()) break;
        if (script_.empty() && screen_ == Screen::Menu && !ui_.gui.top() && input_.key_pressed(ui::vk::kEscape)) break;
    }
    int rc = 0;
    if (!script_.empty()) {
        const bool finished = script_step_ >= script_.size() && !script_failed_;
        std::printf("smoke %s: %zu / %zu steps, %" PRIu64 " frames, %" PRIu64 " battles, galactic AI orders %" PRIu64 "\n", opt_.smoke.c_str(),
                    script_step_, script_.size(), frames_, battles_seen_, session_ ? session_->galactic_ai_orders() : 0);
        if (session_)
            std::printf("session: %" PRIu64 " ticks, orders routed %" PRIu64 " (moves %" PRIu64 ", attacks %" PRIu64 ", rejected %" PRIu64
                        "), commands applied %" PRIu64 " rejected %" PRIu64 "; audio: %" PRIu64 " ui, %" PRIu64 " responses, %" PRIu64
                        " deaths, %" PRIu64 " hits, %" PRIu64 " fires, %" PRIu64 " mode scenes\n",
                        session_->ticks(), session_->order_stats().routed, session_->order_stats().moves, session_->order_stats().attacks,
                        session_->order_stats().rejected, session_->host().commands_applied(), session_->host().commands_rejected(), audio_.stats().ui,
                        audio_.stats().responses, audio_.stats().deaths, audio_.stats().hits, audio_.stats().fires, audio_.stats().scenes);
        std::printf("screens: %" PRIu64 " battle choices, %" PRIu64 " auto-resolve choices, %" PRIu64 " story offers, %" PRIu64
                    " encyclopedia frames, %" PRIu64 " retry boxes, %" PRIu64 " reinforcement drops, holocron %zu chart samples\n",
                    choices_, auto_resolves_, story_dialogs_, ency_shown_, retry_boxes_, reinforce_drops_, holo_.samples());
        std::printf("reveal: %" PRIu64 " planet-revealed sfx, %" PRIu64 " moves to unrevealed planets dropped\n", reveal_sfx_,
                    hidden_moves_dropped_);
        std::printf("hud feeds: %" PRIu64 " reticle frames, %" PRIu64 " reticle orders, %" PRIu64 " Lua GUI calls, %zu galactic heroes, "
                    "reticle table %zu\n",
                    reticle_frames_, reticle_orders_, gui_calls_applied_, galactic_heroes_.size(), reticles_.size());
        std::printf("move acks: %" PRIu64 " (%" PRIu64 " particles spawned, %" PRIu64 " normal acks killed by a double click)\n",
                    move_acks_, move_acks_drawn_, move_ack_kills_);
        // OI host: the camera / controls decisions hold on the tables the run used (scripts never rebind).
        for (const std::string& v : check_control_decisions(ui_.hotkeys, ui_.bindings)) {
            std::fprintf(stderr, "controls: %s\n", v.c_str());
            smoke_ok_ = false;
        }
        if (session_) {
            const PortEventStats& ps = session_->port_stats();
            const SpecialStats& ss = session_->special_stats();
            std::printf("P4 host: %" PRIu64 " auto-resolve results, %" PRIu64 " story cues (%" PRIu64 " speech); withdraw %" PRIu64 " (%" PRIu64
                        " accepted, %" PRIu64 " dropped), battle auto-resolve %" PRIu64 " (%" PRIu64 " refused)\n",
                        auto_results_, story_cues_, audio_.stats().speech, ps.withdraw, ps.withdraw_accepted, ps.withdraw_dropped,
                        ps.tactical_auto_resolve, ps.tactical_auto_resolve_refused);
            std::printf("P4 specials: %" PRIu64 " activations, %" PRIu64 " dialogs, %" PRIu64 " errors, %" PRIu64 " purchases, %" PRIu64
                        " sabotages, %" PRIu64 " hacks, %" PRIu64 " slices (%" PRIu64 " ok), %" PRIu64 " unlocks, %" PRIu64
                        " respawns scheduled / %" PRIu64 " done, %" PRIu64 " SFX, %" PRIu64 " messages\n",
                        activations_, ss.dialogs, ss.errors, ss.purchases, ss.sabotages, ss.hacks, ss.slices, ss.slice_successes,
                        ss.types_unlocked, ss.respawns_scheduled, ss.respawned, special_sfx_, special_msgs_);
            std::printf("OI host: %" PRIu64 " ticks under the cinematic rate lock, %" PRIu64 " fast-forward toggles, %" PRIu64
                        " attract demos, %" PRIu64 " audio flag changes\n",
                        session_->rate_lock_ticks(), fast_toggles_, attract_runs_, audio_.stats().flag_changes);
            std::printf("story UI: %" PRIu64 " raised, %" PRIu64 " sent, %" PRIu64 " applied, %" PRIu64 " dropped; reward media: %" PRIu64
                        " movies, %" PRIu64 " speeches\n",
                        story_ui_.raised_total(), story_ui_sent_, ps.story_ui, ps.story_ui_dropped, reward_movies_,
                        reward_speeches_);  // L1/S4 wave 1
        }
        for (const std::string& n : notes_) std::printf("note: %s\n", n.c_str());
        const bool crc = finished && crc_check();
        if (!finished || !crc || !smoke_ok_) rc = 1;
    }
    shutdown();
    const uint32_t ve = ctx_.validation_errors(), vw = ctx_.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx_.validation_enabled() ? "on" : "off", ve, vw);
    if (ve || vw) rc = 1;
    ctx_.shutdown();
    win_.destroy();
    if (!opt_.smoke.empty()) std::printf(rc == 0 ? "alamo_game smoke %s: OK\n" : "alamo_game smoke %s: FAILED\n", opt_.smoke.c_str());
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    enable_dpi_awareness();  // before any window: sizes and the video modes in physical pixels
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--game") opt.game = next();
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--audio") {
            const std::string m = next();
            if (m == "device") opt.audio = AudioBackendKind::Device;
            else if (m == "null") opt.audio = AudioBackendKind::Null;
            else if (m == "off") opt.audio = AudioBackendKind::Off;
            else return usage();
        } else if (a == "--seed") opt.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--workers") opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--battle-max-frames") opt.battle_max_frames = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--cache-dir") opt.cache_dir = next();
        else if (a == "--png-dir") opt.png_dir = next();
        else if (a == "--smoke") opt.smoke = next();
        else if (a == "--movie") opt.movies.push_back(next());
        else if (a == "--no-intro") opt.intro = false;
        else if (a == "--attract") opt.attract = true;
        else if (a == "--mailbox") opt.mode = rhi::PresentMode::Mailbox, opt.mode_set = true;
        else if (a == "--immediate") opt.mode = rhi::PresentMode::Immediate, opt.mode_set = true;
        else if (a == "--frame-cap") {
            const std::string v = next();
            opt.frame_cap = v == "display" ? ui::kFrameCapDisplay : std::max(0, std::atoi(v.c_str()));
            opt.frame_cap_set = true;
        } else if (a == "--video-options") opt.video_options = next();
        else if (a == "--saves-dir") opt.saves_dir = next();
        else if (a.find('=') != std::string::npos) opt.game_args.push_back(a);
        else return usage();
    }
    if (opt.width == 0 || opt.height == 0) return usage();
    if (!opt.smoke.empty() && opt.smoke != "skirmish" && opt.smoke != "campaign" && opt.smoke != "land") return usage();
    log::set_min_level(log::Level::Warn);
    if (opt.movies.empty() && opt.intro) opt.movies = {"Logo1", "Logo2"};  // Movies.xml names (MAIN_LOOP 5.1 step 13): LucasArtsLogo.bik, PetroglyphLogo.bik
    if (!opt.smoke.empty()) {
        if ((opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game).empty()) {
            std::printf("alamo_game smoke %s: ALAMO_FOC_DATA / --game not set: skipped\n", opt.smoke.c_str());
            return 0;
        }
        // Smoke battles end on the time limit; the skirmish battles get longer for their forces to meet and fight (the
        // space fleets start ~5000 units apart).
        if (opt.battle_max_frames == 27000) opt.battle_max_frames = opt.smoke == "campaign" ? 900u : 4800u;
    }
    App app(std::move(opt));
    return app.run();
}
