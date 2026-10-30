// P4 menus (docs/re/UI.md §3.2 "Screens by dialog"): the front end and the in-game menu as a flow over the GUI
// dialogs, emitting MenuActions for the host (start / load / save / quit, settings). Nothing here touches the sim or
// the save format (N1): save / load are requests.
//
//   IDD_MAIN_MENU_DIALOG            Tutorial -> IDD_SELECT_TUTORIAL_DIALOG, Single Player -> IDD_SINGLE_PLAYER_GAMES_MENU_
//                                   DIALOG, Multiplayer -> OpenMultiplayer, Load Game -> IDD_LOAD_GAME_DIALOG, Options ->
//                                   IDD_MAIN_OPTIONS_DIALOG, Exit -> IDD_DIALOG_BOX_QUIT_CONFIRM -> ExitGame, Credits
//   IDD_SINGLE_PLAYER_GAMES_MENU_   New Campaign -> IDD_CAMPAIGN_SELECTION_DIALOG (story sets, faction A / B, difficulty
//   DIALOG                          slider), Custom Campaign -> IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG (custom sets, faction
//                                   combo, difficulty radios, advanced options), Skirmish -> IDD_SKIRMISH_STAGING_DIALOG
//   IDD_MAIN_OPTIONS_DIALOG /       Audio (IDD_AUDIO_OPTIONS_DIALOG: 4 sliders = AudioBus Master / Sfx / Speech / Music),
//   IDD_IN_GAME_OPTIONS_DIALOG      Video (IDD_VIDEO_OPTIONS_DIALOG -> Advanced: IDD_ADVANCED_VIDEO_OPTIONS_DIALOG ->
//                                   ApplyAdvancedVideo; Auto detect -> AutoDetectVideo), Keyboard (IDD_KEYBOARD_OPTIONS_DIALOG[_2.._5]:
//                                   key-capture edits bound to the P2 HotkeyTable, Accept syncs P1 Bindings), Gameplay
//                                   (IDD_GAMEPLAY_OPTIONS_DIALOG)
//   IDD_GAME_OPTIONS_DIALOG (pause) Save -> IDD_SAVE_GAME_DIALOG, Load -> IDD_LOAD_GAME_DIALOG, Options ->
//                                   IDD_IN_GAME_OPTIONS_DIALOG, Quit -> confirm -> QuitToMainMenu, Resume / Esc
//   IDD_SAVE_GAME_DIALOG /          N1 / t09: the list shows SaveGameInfo::label (provider order); selecting a row puts
//   IDD_LOAD_GAME_DIALOG            its name in the save edit; Save on an existing name (save_exists) and Delete ask on
//                                   IDD_DIALOG_BOX_QUIT_CONFIRM (TEXT_SAVE_CONFIRM / TEXT_DELETE_CONFIRM, the save name
//                                   on IDC_USER_TEXT_STRING_1) before SaveGame / DeleteSave; a save_blocker reason
//                                   disables Save and the edit and shows the reason in it. The host re-lists with
//                                   refresh_saves() after deleting.
//
// [port] choices: campaign menu rules (build_campaign_menu); faction button A / B = the set's first two factions in data
// order (hidden when absent); difficulty 0 easy / 1 normal / 2 hard (slider 0..2, radios); IDC_MAIN_MENU_PLAY_DEMO
// (same rectangle as Single Player), IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON, IDC_MAIN_OPTIONS_MODS, network options and
// custom-map buttons hidden; audio slider moves send PreviewAudio, Cancel reverts; skirmish = 2 players (local + AI);
// key names in the keyboard edits are English VK names with Shift+ / Ctrl+ / Alt+; the IDC_EDIT_* -> hotkey slot table
// is a port table (keyboard_edit_slot) - ability rows are matched by meaning, marked guesses; Tab and Esc are never
// captured; quit-confirm captions use TEXT_BUTTON_YES / TEXT_BUTTON_NO / TEXT_BUTTON_QUIT_GAME.
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/input.hpp"

namespace alamo::data {
class GameDatabase;
struct CampaignDef;
}  // namespace alamo::data

namespace alamo::ui {

// ---- Campaign list from the data layer -------------------------------------------------------------------
struct CampaignChoice {
    std::string campaign;  // CampaignDef name (the one to start)
    std::string faction;   // Starting_Active_Player
};
struct CampaignSetInfo {
    std::string set;                 // Campaign_Set
    std::string text_id, description_text;
    int32_t sort_order = 0;
    bool custom = false;             // Supports_Custom_Settings
    std::vector<CampaignChoice> factions;  // data order
};
struct CampaignMenu {
    std::vector<CampaignSetInfo> story;      // Supports_Custom_Settings false (New Campaign)
    std::vector<CampaignSetInfo> custom;     // Supports_Custom_Settings true (Custom Campaign / galactic conquest)
    std::vector<CampaignSetInfo> tutorials;  // Campaign_Set "Tutorial*"
};
// [port] rules: campaigns without Starting_Active_Player (multiplayer sets) are skipped; Campaign_Set starting with
// "Tutorial" -> tutorials; else story / custom by Supports_Custom_Settings; sets ordered by Sort_Order then first
// appearance (GameDatabase::campaigns_by_sort_order), factions in data order.
CampaignMenu build_campaign_menu(const data::GameDatabase& db);
CampaignMenu build_campaign_menu(std::span<const data::CampaignDef* const> sorted);

// ---- Settings exchanged with the host --------------------------------------------------------------------
struct AudioVolumes {  // audio::AudioBus Master / Sfx / Speech / Music, 0..1
    float master = 1.0f, sfx = 1.0f, speech = 1.0f, music = 1.0f;
    bool subtitles = true;
    friend bool operator==(const AudioVolumes&, const AudioVolumes&) = default;
};
// [port: Gabriel] the swap mode IDC_VSYNC_ENABLED uses when checked. IDC_PORT_VSYNC_MODE lists Off (Immediate) first, then
// these (row = mode + 1), and the dialog keeps the two controls in step (U9): Off <-> unchecked. FIFO (waits for the
// display's refresh) or Mailbox (no tearing, the newest finished frame is shown: lower latency, the GPU never waits).
enum class VsyncMode : uint8_t { Fifo, Mailbox };
inline constexpr size_t kVsyncModeCount = 2;
// [port: Gabriel] IDC_PORT_FRAME_CAP rows: frames per second, 0 = no cap, kFrameCapDisplay = the monitor's refresh rate.
// The cap holds with v-sync on too (e.g. 60 fps on a 144 Hz display with FIFO, or Mailbox capped at the refresh rate).
inline constexpr int32_t kFrameCapDisplay = -1;
inline constexpr int32_t kFrameCapChoices[] = {0, kFrameCapDisplay, 30, 60, 90, 120, 144, 165, 240};
struct VideoSettings {
    std::vector<std::u32string> resolutions;  // list items ("1920 x 1080")
    int32_t resolution = -1;
    bool windowed = false, vsync = true, widescreen = true, hardware_mouse = false;
    VsyncMode vsync_mode = VsyncMode::Fifo;  // [port: Gabriel] IDC_PORT_VSYNC_MODE
    bool mailbox_available = true;           // false: the Mailbox row says the driver lacks it (FIFO is used)
    int32_t frame_cap = 0;                   // [port: Gabriel] IDC_PORT_FRAME_CAP: one of kFrameCapChoices
    std::u32string display_info;             // [port: Gabriel] IDC_PORT_DISPLAY_INFO ("160 Hz, Mailbox"); empty = blank
    int32_t detail = 100, anti_alias = 0, gamma = 50;  // slider values 0..100
    // IDC_DEVICE_DESC_STATIC (top right): the display adapter's name; its RC caption TEXT_VIDEO_RESOLUTION is a
    // placeholder the original overwrites, so empty leaves the field blank [port].
    std::u32string device;
    // [port: Gabriel] t12 (playtest 3): true on the ApplyVideo that follows a resolution change while the "keep these
    // settings?" dialog is open - the host applies it but must not store the mode yet. The Keep answer sends the same
    // settings again with this false (the host saves); a Revert / timeout sends the previous resolution, also false.
    bool unconfirmed = false;
};
// [port: Gabriel] t12: seconds the resolution-change confirmation waits before it reverts (the original's countdown,
// TEXT_REVERTING_IN_X_SECONDS, is not in the recovered specs: 15 s as Windows' own display dialog [M] guess).
inline constexpr double kResolutionConfirmSeconds = 15.0;
// IDD_ADVANCED_VIDEO_OPTIONS_DIALOG (from IDD_VIDEO_OPTIONS_DIALOG's Advanced button): 7 detail sliders 0..100 + 3 checks.
// [port: Gabriel] plus the port's anti-aliasing combo (IDC_PORT_ANTI_ALIASING), ambient occlusion check
// (IDC_PORT_AMBIENT_OCCLUSION) and anisotropic filtering combo (IDC_PORT_ANISOTROPY) from kPortDialogExtensionsRc.
enum class AntiAliasing : uint8_t { Off, Taa };
inline constexpr size_t kAntiAliasingCount = 2;
inline constexpr int32_t kAnisotropyChoices[] = {0, 2, 4, 8, 16};  // 0 = off (the materials' own sampler state)
// [port] t19 / D9: the Video dialog's Anti-aliasing slider and the Advanced dialog's combo are one setting: slider 0 = Off,
// above 0 = TAA at that strength. Turning TAA on from the combo starts at kAntiAliasDefaultSlider, the slider position
// closest to the tuned current-frame weight of 0.1 [inferred: the original has no such combo].
inline constexpr int32_t kAntiAliasDefaultSlider = 63;
constexpr AntiAliasing anti_aliasing_from_slider(int32_t slider) { return slider > 0 ? AntiAliasing::Taa : AntiAliasing::Off; }
constexpr int32_t anti_alias_slider_for(AntiAliasing mode, int32_t slider) {
    return mode == AntiAliasing::Off ? 0 : slider > 0 ? slider : kAntiAliasDefaultSlider;
}
struct AdvancedVideoSettings {
    int32_t particle = 100, mesh = 100, texture = 100, water = 100, shader = 100, shadow = 100, environment = 100;
    bool soft_shadows = true, heat_distortion = true, bloom = true;
    AntiAliasing anti_aliasing = AntiAliasing::Off;
    bool ambient_occlusion = false;
    int32_t anisotropy = 0;  // one of kAnisotropyChoices
    friend bool operator==(const AdvancedVideoSettings&, const AdvancedVideoSettings&) = default;
};
// t43 [port]: the Video dialog's Detail slider and the seven advanced detail sliders agree both ways. A moved Detail slider
// is a preset for the seven; the seven give Detail their rounded mean, so equal sliders give that value back exactly
// [inferred: what the original's Detail slider shows after a custom mix is not recovered].
constexpr int32_t advanced_detail_level(const AdvancedVideoSettings& s) {
    return (s.particle + s.mesh + s.texture + s.water + s.shader + s.shadow + s.environment + 3) / 7;
}
constexpr void set_advanced_detail(AdvancedVideoSettings& s, int32_t detail) {
    s.particle = s.mesh = s.texture = s.water = s.shader = s.shadow = s.environment = detail;
}
// [port: Gabriel] camera / controls settings (docs/PHASE4.md "Camera and controls"): galactic and tactical field of view
// (degrees: galactic horizontal, tactical as TacticalCameras.xml - defined for 4:3) for GalacticCameraController::
// set_fov_deg / TacticalCameraController::set_fov_override, and the queue modifier (Bindings::set_queue_mods).
struct CameraSettings {
    float galactic_fov_deg = 26.565f;  // atan(0.5), the original
    float tactical_fov_deg = 55.0f;
    uint8_t queue_mods = kModShift;
    friend bool operator==(const CameraSettings&, const CameraSettings&) = default;
};
inline constexpr float kGalacticFovMinDeg = 15.0f, kGalacticFovMaxDeg = 60.0f, kTacticalFovMinDeg = 30.0f,
                       kTacticalFovMaxDeg = 110.0f;

// [port: Gabriel] the port's own dialog IDD_PORT_CAMERA_CONTROLS_DIALOG (FOV sliders, key capture for Pause and the
// camera yaw / pitch / roll hotkey slots, queue-modifier combo). Merge it into the script after GUIDialogs.rc
// (DialogScript::merge); the options menus then show it in the slot of the hidden Mods / Network option.
extern const char* const kPortDialogsRc;
inline constexpr const char* kPortControlsDialog = "IDD_PORT_CAMERA_CONTROLS_DIALOG";
// [port: Gabriel] controls the port adds to shipped dialogs: IDD_VIDEO_OPTIONS_DIALOG gets the V-sync mode combo
// (IDC_PORT_VSYNC_MODE), the frame rate cap combo (IDC_PORT_FRAME_CAP), a live 0..100 value beside each slider's title
// (IDC_PORT_ANTI_ALIAS_VALUE, _DETAIL_VALUE, _GAMMA_VALUE; the shipped Low / Off / Max captions only name the slider's ends)
// and a display line (IDC_PORT_DISPLAY_INFO: refresh rate and the swap mode in use) under the
// Advanced button; IDD_ADVANCED_VIDEO_OPTIONS_DIALOG gets the anti-aliasing / anisotropic filtering combos and the
// ambient occlusion check (AdvancedVideoSettings). Add with DialogScript::extend after GUIDialogs.rc is loaded.
extern const char* const kPortDialogExtensionsRc;

// IDC_SCROLL_SPEED_SLIDER: the dialog slider runs 0..100; the original option is 0..3 with Defaults = 1.0
// (P1_CAMERA_INPUT.md §2.1, §3.1). The port maps it piecewise linearly so the default position is exactly 1.0:
// 0..kDefaultScrollSpeedSlider -> 0..1, kDefaultScrollSpeedSlider..100 -> 1..3 (clamped). Feed the result to
// CameraTuning::scroll_speed_option (the camera uses at least 0.1).
inline constexpr int32_t kDefaultScrollSpeedSlider = 33;
float scroll_speed_option_from_slider(int32_t slider);

struct GameplaySettings {
    int32_t game_speed = 50, strategic_speed = 50, scroll_speed = kDefaultScrollSpeedSlider;  // 0..100
    int32_t difficulty = 1;  // 0 easy, 1 normal, 2 hard
    // absolute_scroll: IDC_ABSOLUTE_SCROLLING, on by default (§3.1). The Defaults button restores these values.
    bool ask_autoresolve = true, delay_tooltips = false, absolute_scroll = true, alternate_mouse = false,
         right_button_scroll = false, both_buttons_rotate = false, simplified_colors = false, abilities_autofire = false;
};
struct SaveGameInfo {
    std::string name;      // save identifier (host)
    std::u32string label;  // list text
};
struct SkirmishMapInfo {
    std::string map;       // map file / id
    std::u32string label;
    uint32_t max_players = 2;
};

struct MenuAction {
    enum class Kind : uint8_t {
        StartCampaign,   // campaign, faction, difficulty
        StartSkirmish,   // map, factions (player order), difficulty
        StartTutorial,   // campaign
        LoadGame,        // save
        SaveGame,        // save (name typed or selected)
        DeleteSave,      // save
        ResumeGame,
        QuitToMainMenu,
        ExitGame,
        OpenMultiplayer,
        ShowCredits,
        PreviewAudio,    // audio (slider moved; live preview)
        ApplyAudio,      // audio
        ApplyVideo,      // video
        ApplyGameplay,   // gameplay
        KeybindingsChanged,  // the HotkeyTable (and Bindings) were updated
        // P4 completion (2026-10-27), appended:
        ApplyAdvancedVideo,  // advanced
        AutoDetectVideo,     // Auto detect on the video / advanced video dialogs
        ApplyCamera,         // [port: Gabriel] camera (FOVs, queue modifier); hotkeys changed too -> KeybindingsChanged
    };
    Kind kind = Kind::ResumeGame;
    std::string campaign, faction, map, save;
    std::vector<std::string> factions;
    int32_t difficulty = 1;
    AudioVolumes audio;
    VideoSettings video;
    GameplaySettings gameplay;
    AdvancedVideoSettings advanced;
    CameraSettings camera;
};
const char* menu_action_name(MenuAction::Kind k);

struct MenuProviders {
    std::function<std::vector<SaveGameInfo>()> saves;
    // N1 / t09: save_blocker = "" when the game can be saved now, else why not (asked when the Save dialog opens);
    // save_exists = a save of this name exists (Save then asks to overwrite). Null: always allowed / never asks.
    std::function<std::u32string()> save_blocker;
    std::function<bool(const std::string&)> save_exists;
    std::function<std::vector<SkirmishMapInfo>()> skirmish_maps;
    std::function<AudioVolumes()> audio;
    std::function<VideoSettings()> video;
    std::function<GameplaySettings()> gameplay;
    std::function<AdvancedVideoSettings()> advanced_video;  // null: defaults
    std::function<CameraSettings()> camera_settings;        // [port: Gabriel] null: defaults
    HotkeyTable* hotkeys = nullptr;  // edited by the keyboard dialogs (Accept)
    Bindings* bindings = nullptr;    // synced with apply_to_bindings on Accept
    std::vector<std::string> skirmish_factions{"Rebel", "Empire", "Underworld"};
    bool demo = false;               // show IDC_MAIN_MENU_PLAY_DEMO instead of Single Player
};

// Keyboard dialogs: IDC_EDIT_* control -> hotkey slot (-1 when the port table has no entry).
int32_t keyboard_edit_slot(std::string_view control_id);
// "Ctrl+A", "F12", "Numpad -", "" for unbound.
std::u32string hotkey_label(const HotkeyBinding& b);

class FrontEnd {
public:
    void init(GuiManager& gui, CampaignMenu menu, MenuProviders providers);
    void show_main_menu();
    void show_pause_menu();       // in-game menu (IDD_GAME_OPTIONS_DIALOG) over the HUD
    void close_pause_menu();
    bool pause_menu_open() const;
    bool in_game() const { return in_game_; }
    void set_in_game(bool g) { in_game_ = g; }
    const CampaignMenu& campaigns() const { return menu_; }

    // Consumes this frame's GUI events (and, for the key-capture fields, the raw key presses). Call after
    // GuiManager::update.
    void update(std::span<const GuiEvent> events, const InputFrame& raw, std::vector<MenuAction>& out);
    // Dialog the flow is on (topmost), "" when none.
    std::string top() const;
    // [port: Gabriel] t12: the "keep these settings?" confirmation after a resolution change (IDD_DIALOG_BOX2_CENTERED with
    // TEXT_RESOLUTION_CHANGED_CONFIRMATION and a TEXT_REVERTING_IN_X_SECONDS countdown on InputFrame::time). Yes keeps, No /
    // Esc / the timeout re-apply the previous resolution (an ApplyVideo that the host also stores).
    bool resolution_confirm_open() const { return res_confirm_; }
    // N1 / t09: re-reads the saves provider into an open save / load dialog (the host deleted or wrote a save).
    void refresh_saves();
    double resolution_confirm_remaining() const { return res_confirm_ ? std::max(0.0, res_deadline_ - now_) : 0.0; }

private:
    void go(std::string_view dialog);    // replace the top dialog
    GuiDialog* push(std::string_view dialog);
    void pop();
    void on_click(const GuiEvent& e, std::vector<MenuAction>& out);
    void on_cancel(const GuiEvent& e, std::vector<MenuAction>& out);
    void on_change(const GuiEvent& e, std::vector<MenuAction>& out);
    void setup(GuiDialog& d);
    void fill_campaign_select(GuiDialog& d);
    void fill_custom_staging(GuiDialog& d);
    void fill_skirmish(GuiDialog& d);
    void fill_saves(GuiDialog& d);
    void fill_audio(GuiDialog& d, const AudioVolumes& v);
    void fill_video(GuiDialog& d);
    void fill_advanced_video(GuiDialog& d, const AdvancedVideoSettings& s);
    AdvancedVideoSettings read_advanced_video(const GuiDialog& d) const;
    void fill_port_controls(GuiDialog& d, const CameraSettings& s);
    CameraSettings read_port_controls(const GuiDialog& d) const;
    void fill_gameplay(GuiDialog& d, const GameplaySettings& g);
    void fill_keyboard(GuiDialog& d);
    AudioVolumes read_audio(const GuiDialog& d) const;
    GameplaySettings read_gameplay(const GuiDialog& d) const;
    int32_t difficulty_of(const GuiDialog& d) const;
    std::u32string text(std::string_view key) const;
    std::u32string text_or(std::string_view key, std::u32string fallback) const;  // the text database's entry, else fallback
    void begin_resolution_confirm(const VideoSettings& applied, int32_t previous_resolution, std::vector<MenuAction>& out);
    void end_resolution_confirm(bool keep, std::vector<MenuAction>& out);
    void show_confirm_countdown(GuiDialog& d);
    std::u32string faction_label(std::string_view faction) const;

    GuiManager* gui_ = nullptr;
    CampaignMenu menu_;
    MenuProviders p_;
    bool in_game_ = false;
    AudioVolumes audio_before_;
    HotkeyTable keys_work_;  // keyboard dialogs edit a copy; Accept writes it back
    bool keys_dirty_ = false;
    std::vector<SaveGameInfo> saves_;
    std::vector<SkirmishMapInfo> maps_;
    std::string quit_action_;  // "exit" / "main"; N1 / t09: "overwrite" / "delete" (confirm_save_)
    std::string confirm_save_;  // the save an overwrite / delete confirmation is about
    bool save_blocked_ = false; // the open Save dialog's save_blocker refused
    int32_t vsync_on_row_ = 1;  // U9: the V-sync combo row that re-checking IDC_VSYNC_ENABLED returns to
    bool video_status_stale_ = false;  // an ApplyVideo went out: re-read IDC_PORT_DISPLAY_INFO once the host has handled it
    bool res_confirm_ = false;  // t12: the confirmation dialog is open
    double res_deadline_ = 0.0, now_ = 0.0;
    int32_t res_shown_ = -1;    // seconds last written into the countdown line
    VideoSettings res_keep_, res_revert_;
};

}  // namespace alamo::ui
