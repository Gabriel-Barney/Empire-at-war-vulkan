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
//   IDD_IN_GAME_OPTIONS_DIALOG      Display (the Video button: IDD_VIDEO_OPTIONS_DIALOG -> ApplyVideo), Graphics ([port:
//                                   Gabriel] button: IDD_ADVANCED_VIDEO_OPTIONS_DIALOG -> ApplyAdvancedVideo, plus ApplyVideo
//                                   when its Anti-Aliasing list changed; the shipped Auto Detect button is hidden), Keyboard (IDD_KEYBOARD_OPTIONS_DIALOG[_2.._5]:
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
//                                   [port: Gabriel 2026-11-13] the list is a tree: SaveGameInfo::groups nests each save in
//                                   groups (rows "[+] label  (count)", indented one row height per level, the group's icon);
//                                   a mouse click on a group row (Load on a selected one) opens / closes it, the newest
//                                   save's groups open each time the dialog fills. Save rows show SaveGameInfo::icon.
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
#include <utility>
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
// [port: Gabriel 2026-11-13] IDC_PORT_WINDOW_MODE rows (in this order), replacing the shipped "Run in window" check
// (IDC_WINDOWED_MODE, hidden): Fullscreen = the monitor takes the picked resolution and a frameless window covers it
// [port: no exclusive Vulkan full screen]; Borderless = a frameless window over the monitor at the desktop's own resolution
// (the picked resolution's refresh rate is still applied, its size is not); Windowed = a framed window of the picked size.
enum class WindowMode : uint8_t { Fullscreen, Borderless, Windowed };
inline constexpr size_t kWindowModeCount = 3;
// [port: Gabriel 2026-11-13] IDC_PORT_UI_SCALE rows: DPI-style UI scale in percent (HudCanvas::make). 100 % = one canvas
// unit per physical pixel; the UI never grows past the original's fit-the-screen-height size (a larger choice shows that).
// The game starts at 100 % whatever the Windows display scale is (the process is per-monitor DPI aware).
inline constexpr int32_t kUiScaleChoices[] = {100, 125, 150, 175, 200, 225, 250, 275, 300, 350, 400, 450, 500};
inline constexpr int32_t kUiScaleMin = 100, kUiScaleMax = 500;
// [port: Gabriel 2026-11-13] Anti-aliasing modes (VideoSettings::anti_alias; the list kAntiAliasChoices below).
inline constexpr int32_t kAaOff = 0, kAaSmaa = 1, kAaTaa = 2, kAaMsaa2 = 3, kAaMsaa4 = 4, kAaMsaa8 = 5;
// [port: Gabriel 2026-11-13] FSR upscaling modes, one per FSR quality mode in render::UpscaleQuality order (mode - kAaFsrNative).
inline constexpr int32_t kAaFsrNative = 6, kAaFsrQuality = 7, kAaFsrBalanced = 8, kAaFsrPerformance = 9, kAaFsrUltra = 10;
struct VideoSettings {
    std::vector<std::u32string> resolutions;  // list items ("1920 x 1080")
    int32_t resolution = -1;
    WindowMode window_mode = WindowMode::Fullscreen;  // [port: Gabriel] IDC_PORT_WINDOW_MODE
    int32_t ui_scale = 100;                           // [port: Gabriel] IDC_PORT_UI_SCALE, one of kUiScaleChoices
    bool vsync = true;
    VsyncMode vsync_mode = VsyncMode::Fifo;  // [port: Gabriel] IDC_PORT_VSYNC_MODE
    bool mailbox_available = true;           // false: the Mailbox row says the driver lacks it (FIFO is used)
    int32_t frame_cap = 0;                   // [port: Gabriel] IDC_PORT_FRAME_CAP: one of kFrameCapChoices
    std::u32string display_info;             // [port: Gabriel] IDC_PORT_DISPLAY_INFO ("160 Hz, Mailbox"); empty = blank
    int32_t detail = 100, gamma = 50;  // 0..100: Detail is picked from a list (kQualityPresets), Gamma is a slider
    int32_t anti_alias = kAaOff;       // [port: Gabriel] the Anti-aliasing list's mode (kAntiAliasChoices: kAaOff .. kAaMsaa8)
    // Set on the ApplyVideo where the player picked a quality level (Low .. Ultra) the options did not already have: `detail`
    // is then that level's value and the host sets the seven advanced detail options to it (detail_preset). Left false when
    // the list still says what the options say, or says Custom, so a hand-made mix is never overwritten by an Apply.
    bool detail_picked = false;
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
// IDD_ADVANCED_VIDEO_OPTIONS_DIALOG (the Graphics page since the Display / Graphics split): 7 detail options + 3 checks.
// [port: Gabriel] plus the port's ambient occlusion check (IDC_PORT_AMBIENT_OCCLUSION) and anisotropic filtering combo
// (IDC_PORT_ANISOTROPY) from kPortDialogExtensionsRc. Anti-aliasing is VideoSettings::anti_alias (its list is on this page).
inline constexpr int32_t kAnisotropyChoices[] = {0, 2, 4, 8, 16};  // 0 = off (the materials' own sampler state)
// [port: Gabriel] Render Scale combo (IDC_PORT_RENDER_SCALE): percent of the window's pixels the 3D scene is drawn with
// (render::RecordOptions::render_scale; 75 at 3840 x 2160 = 3326 x 1871). 100 = native. HUD and dialogs stay native.
inline constexpr int32_t kRenderScaleChoices[] = {50, 60, 67, 75, 85, 100};

// [port: Gabriel] Named levels for the options that were 0..100 sliders. The stored values stay 0..100 (the profile file and
// the renderer's mappings in game_display.hpp are unchanged); a list shows the level a stored value falls in, and picking a
// level stores its `value`. `from` is the lowest stored value that level covers, so a value from an older profile or a
// slider-era preset lands on the level the renderer already treated it as.
struct LevelChoice {
    int32_t value;
    int32_t from;
    const char32_t* label;
};
// [port: Gabriel 2026-11-13] Anti-aliasing: one list, one choice - Off, SMAA, TAA, MSAA 2x / 4x / 8x - each at one fixed
// quality (game_display.hpp apply_aa_options; the original has no anti-aliasing option). The stored value is the mode below
// (VideoSettings::anti_alias, profile key "aa_mode"). An MSAA count the device cannot do is drawn with the largest one it can
// (render::Renderer::msaa_samples), so the list is the same on every device. The FSR rows draw the scene at a lower
// resolution and upscale it with AMD FSR (4.1 where the GPU supports it, else 3.1); where FSR cannot run they draw TAA.
inline constexpr LevelChoice kAntiAliasChoices[] = {{kAaOff, kAaOff, U"Off"},       {kAaSmaa, kAaSmaa, U"SMAA"},
                                                    {kAaTaa, kAaTaa, U"TAA"},       {kAaMsaa2, kAaMsaa2, U"MSAA 2x"},
                                                    {kAaMsaa4, kAaMsaa4, U"MSAA 4x"}, {kAaMsaa8, kAaMsaa8, U"MSAA 8x"},
                                                    {kAaFsrNative, kAaFsrNative, U"FSR Native AA"},
                                                    {kAaFsrQuality, kAaFsrQuality, U"FSR Quality (1.5x)"},
                                                    {kAaFsrBalanced, kAaFsrBalanced, U"FSR Balanced (1.7x)"},
                                                    {kAaFsrPerformance, kAaFsrPerformance, U"FSR Performance (2x)"},
                                                    {kAaFsrUltra, kAaFsrUltra, U"FSR Ultra Performance (3x)"}};
// The seven detail options. Four levels where the renderer has four steps (Shader) or a continuous scale (Particle, Mesh,
// Environment); three where it has three (Texture: top mips skipped 2 / 1 / 0, Water: reflection targets 0 / 1 / 2);
// Shadow is on or off (any value above 0 draws shadows). Each list ends at the value the old slider's Max stood for.
inline constexpr LevelChoice kLevels4[] = {{0, 0, U"Low"}, {40, 17, U"Medium"}, {75, 50, U"High"}, {100, 84, U"Ultra"}};
inline constexpr LevelChoice kLevels3[] = {{0, 0, U"Low"}, {40, 25, U"Medium"}, {100, 75, U"High"}};
inline constexpr LevelChoice kTextureChoices[] = {
    {0, 0, U"Low - quarter size"}, {40, 25, U"Medium - half size"}, {100, 75, U"High - full size"}};
inline constexpr LevelChoice kShadowChoices[] = {{0, 0, U"Off"}, {100, 1, U"On"}};
// The index of the level a stored value falls in.
constexpr size_t level_index(std::span<const LevelChoice> choices, int32_t value) {
    size_t k = 0;
    for (size_t i = 1; i < choices.size(); ++i)
        if (value >= choices[i].from) k = i;
    return k;
}

struct AdvancedVideoSettings {
    int32_t particle = 100, mesh = 100, texture = 100, water = 100, shader = 100, shadow = 100, environment = 100;
    bool soft_shadows = true, heat_distortion = true, bloom = true;
    bool ambient_occlusion = false;
    int32_t anisotropy = 0;  // one of kAnisotropyChoices
    int32_t render_scale = 100;  // percent of the window's pixels, one of kRenderScaleChoices (a stored other value is kept)
    friend bool operator==(const AdvancedVideoSettings&, const AdvancedVideoSettings&) = default;
};
// The overall quality levels of the Video dialog's Detail Level list: each one sets all seven detail options to its value
// (the checks, anti-aliasing and anisotropy are left alone). A mix that is none of them reads as Custom.
inline constexpr int32_t kQualityPresets[] = {0, 40, 75, 100};  // Low, Medium, High, Ultra
inline constexpr size_t kQualityCount = 4;
inline constexpr size_t kQualityCustom = kQualityCount;  // the list's last row
// t43 [port]: the Detail value kept in the profile is the rounded mean of the seven, so equal options give back exactly that
// value. [inferred: what the original's Detail slider shows after a custom mix is not recovered].
constexpr int32_t advanced_detail_level(const AdvancedVideoSettings& s) {
    return (s.particle + s.mesh + s.texture + s.water + s.shader + s.shadow + s.environment + 3) / 7;
}
constexpr void set_advanced_detail(AdvancedVideoSettings& s, int32_t detail) {
    s.particle = s.mesh = s.texture = s.water = s.shader = s.shadow = s.environment = detail;
}
// Which quality level the seven options make: the preset whose levels they all fall in, else kQualityCustom.
constexpr size_t quality_level(const AdvancedVideoSettings& s) {
    for (size_t p = 0; p < kQualityCount; ++p) {
        const int32_t v = kQualityPresets[p];
        if (level_index(kLevels4, s.particle) == level_index(kLevels4, v) && level_index(kLevels4, s.mesh) == level_index(kLevels4, v) &&
            level_index(kLevels4, s.shader) == level_index(kLevels4, v) && level_index(kLevels4, s.environment) == level_index(kLevels4, v) &&
            level_index(kLevels3, s.water) == level_index(kLevels3, v) && level_index(kTextureChoices, s.texture) == level_index(kTextureChoices, v) &&
            level_index(kShadowChoices, s.shadow) == level_index(kShadowChoices, v))
            return p;
    }
    return kQualityCustom;
}
// [port: Gabriel] camera / controls settings (docs/DECISIONS.md "Camera and controls"): galactic and tactical field of view
// (degrees: galactic horizontal, tactical as TacticalCameras.xml - defined for 4:3) for GalacticCameraController::
// set_fov_deg / TacticalCameraController::set_fov_override, and the queue modifier (Bindings::set_queue_mods).
struct CameraSettings {
    float galactic_fov_deg = 26.565f;  // atan(0.5), the original
    float tactical_fov_deg = 55.0f;
    uint8_t queue_mods = kModShift;
    // [port: Gabriel, 2026-11-13] IDC_PORT_SHOW_PATH_LINES: draw the planned route (the sim's path, unit to destination)
    // of every moving unit of the local player, one line per squadron / company, in tactical battles (space and land), as in the "Empire at War Remake" mod. Off by default: the original
    // only draws the line of a queued waypoint path (UI.md 1.5).
    bool show_path_lines = false;
    friend bool operator==(const CameraSettings&, const CameraSettings&) = default;
};
inline constexpr float kGalacticFovMinDeg = 15.0f, kGalacticFovMaxDeg = 60.0f, kTacticalFovMinDeg = 30.0f,
                       kTacticalFovMaxDeg = 110.0f;

// [port: Gabriel] the port's own dialog IDD_PORT_CAMERA_CONTROLS_DIALOG (FOV sliders, key capture for Pause and the
// camera yaw / pitch / roll hotkey slots, queue-modifier combo). Merge it into the script after GUIDialogs.rc
// (DialogScript::merge); the options menus then show it in the slot of the hidden Mods / Network option.
// modc t08: the same script holds IDD_PORT_MODS_DIALOG, the mod picker (installed mods left, the selected one's load
// report right, Back / Launch), opened by the port's IDC_PORT_MAIN_MENU_MODS button that kPortDialogExtensionsRc adds to
// the main menu under Exit (shown when MenuProviders::mods is set and the dialog is merged).
extern const char* const kPortDialogsRc;
inline constexpr const char* kPortControlsDialog = "IDD_PORT_CAMERA_CONTROLS_DIALOG";
inline constexpr const char* kPortModsDialog = "IDD_PORT_MODS_DIALOG";
// [port: Gabriel] controls the port adds to shipped dialogs: IDD_VIDEO_OPTIONS_DIALOG gets the V-sync mode combo
// (IDC_PORT_VSYNC_MODE), the frame rate cap combo (IDC_PORT_FRAME_CAP), a live 0..100 value beside the Gamma slider
// (IDC_PORT_GAMMA_VALUE; the shipped Low / Max captions only name the slider's ends) and a display line
// (IDC_PORT_DISPLAY_INFO: refresh rate and the swap mode in use), the display mode list (IDC_PORT_WINDOW_MODE,
// WindowMode) in place of the hidden IDC_WINDOWED_MODE check and the UI scale list (IDC_PORT_UI_SCALE, kUiScaleChoices).
// Display / Graphics split (Gabriel, 2026-11-13): the options menus' Video button is Display and opens IDD_VIDEO_OPTIONS_DIALOG
// with the display settings only (its Anti-Aliasing and Detail Level rows, Advanced and Auto Detect buttons are hidden, the
// rows under them move up); a Graphics button the port adds under it (IDC_PORT_MAIN_OPTIONS_GRAPHICS /
// IDC_PORT_IN_GAME_OPTIONS_GRAPHICS) opens IDD_ADVANCED_VIDEO_OPTIONS_DIALOG as the Graphics page. That page gets the
// Detail Level list (IDC_PORT_DETAIL_LEVEL: Low / Medium / High / Ultra / Custom, a preset for the seven lists, see
// kQualityPresets), a list in place of each of the seven detail sliders (IDC_PORT_<NAME>_DETAIL, the level lists above), the
// Anti-Aliasing list (IDC_PORT_ANTI_ALIASING: kAntiAliasChoices), the render scale and anisotropic filtering combos and the
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
// ui1 t06: IDD_CAMPAIGN_ADVANCED_OPTIONS_DIALOG = the game options record (SKIRMISH_SETUP.md, GMM+0xbc: +0 start credits,
// +4 start tech, +8 max tech, +0xc auto-resolve 0 always / 1 never / 2 ask / 3 overwhelming odds (BATTLE_TRANSITIONS §1.6),
// flag 0x10 random events); defaults GameConstants MP_Default_Credits / _Start_Tech_Level / _Max_Tech_Level /
// _Allow_Auto_Resolve / _Allow_Random_Events. [port] the credits slider runs Min_Skirmish_Credits..Max_Skirmish_Credits
// in steps of 1000 (the original's range is not recovered); the tech sliders 1..5, max tech >= start tech.
inline constexpr int32_t kAutoResolveAlways = 0, kAutoResolveNever = 1, kAutoResolveAsk = 2, kAutoResolveOdds = 3;
struct GameOptionsSetup {
    int32_t credits = 6000, start_tech = 1, max_tech = 5, auto_resolve = kAutoResolveAsk;
    bool random_events = false;
    int32_t min_credits = 2000, max_credits = 8000;  // the slider range (host)
    bool set = false;  // Accept was pressed: the game uses these (else the campaign's own values)
    friend bool operator==(const GameOptionsSetup&, const GameOptionsSetup&) = default;
};
// [port: Gabriel] one level of the saved games tree (SaveGameInfo::groups, outermost first). Saves with the same keys up to
// a level share that level's group; its row shows the first such save's label and icon.
struct SaveGroupLevel {
    std::string key;       // the group among its siblings
    std::u32string label;  // group row text
    std::string icon{};    // group row icon texture ("" = none)
};
struct SaveGameInfo {
    std::string name;      // save identifier (host)
    std::u32string label;  // list text
    std::string icon{};    // [port: Gabriel] row icon texture (the save's faction; "" = none)
    std::vector<SaveGroupLevel> groups{};  // [port: Gabriel] the groups the save is listed in (empty = top level)
};
struct SkirmishMapInfo {
    std::string map;       // map file / id (what StartSkirmish carries)
    std::u32string label;  // list text; empty = the row text is built from name_key / players / map (FrontEnd, UI.md 9.3 / 9.4)
    uint32_t max_players = 2;
    // p4ui t06 (docs/re/UI.md section 9). The skirmish staging list shows the rows of one tab (`custom`), space maps first.
    std::string name_key{}; // the map's name text key (UTF-8, mini 08), "" = none: the row shows the file name
    uint32_t players = 0;   // player count for the "(N) " prefix, 0 = use max_players
    bool custom = false;    // Custom tab (true) or Official tab (false)
    bool space = false;     // a space map: listed under the Space game type, else under Land (p4uim t01)
    // ccpgt t02 (UI.md 9.4 / 9.7): a land map whose game-mode tag (mini 0A) is CONTROL: also listed under Capture Control Points.
    bool control = false;
};
// The skirmish game types of IDC_GAME_TYPE_LIST (UI.md 9.4 / 9.7: the retail item data).
inline constexpr int32_t kSkirmishGameLand = 2, kSkirmishGameSpace = 3, kSkirmishGameControl = 4;
// p4uim t03 (docs/re/UI.md section 9.6): what IDC_IMAGE shows for the selected map. `texture` is the image the host chose
// (<stem>.tga, else the map's embedded image, else Generic_Space / Generic_Land.tga; "" = none); `starts` are the start
// positions as 0..1 fractions of the image (x right, y down), drawn over it as the numbers 1..N when the map has them
// (mini 12, UI.md 9.6.1).
struct SkirmishPreview {
    std::string texture;
    std::vector<Vec2f> starts;
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
        ApplyCamera,         // [port: Gabriel] camera (FOVs, queue modifier); hotkeys changed too -> KeybindingsChanged
        // modc t08 (IDD_PORT_MODS_DIALOG), appended:
        CheckMod,   // mod: the picker row selected (ModPickerEntry::id; "" = the base game) wants its load report
        LaunchMod,  // mod: restart with that mod (the host builds the MODPATH= / STEAMMOD= arguments)
    };
    Kind kind = Kind::ResumeGame;
    std::string campaign, faction, map, save;
    std::string mod;  // CheckMod / LaunchMod
    std::vector<std::string> factions;
    // StartSkirmish (p4uia t02): the colour index (0..8, a GameConstants MP_Color_* entry) of each player, in player order.
    std::vector<int32_t> colors;
    // StartSkirmish (ui1 t07): per playing slot in slot order, its team (1-based); factions / colors list the same slots
    // (slot 0 = the local player, 1 = the first AI, then the AI slots the player opened).
    std::vector<int32_t> teams;
    // StartSkirmish (ccpgt t02): the game type selected (kSkirmishGameLand / Space / Control).
    int32_t game_type = kSkirmishGameLand;
    int32_t difficulty = 1;
    AudioVolumes audio;
    VideoSettings video;
    GameplaySettings gameplay;
    AdvancedVideoSettings advanced;
    CameraSettings camera;
    GameOptionsSetup game_options;  // ui1 t06: StartCampaign / StartSkirmish (set = Accepted on the advanced options)
};
const char* menu_action_name(MenuAction::Kind k);

// modc t08: one row of the mod picker (IDD_PORT_MODS_DIALOG). The host lists the base game first (id "") and every
// installed mod (alamo_game: game::list_installed_mods).
struct ModPickerEntry {
    std::string id;         // what CheckMod / LaunchMod report ("" = the base game, no mod)
    std::u32string label;   // the row text
    bool active = false;    // the data loaded now (the row is marked; Launch is off for it)
};
struct SkirmishFaction {
    std::string name;     // Factions.xml Name (what StartSkirmish reports)
    std::string text_id;  // Text_ID ("" = the TEXT_FACTION_<NAME> guess, then the name)
};
struct MenuProviders {
    std::function<std::vector<SaveGameInfo>()> saves;
    // N1 / t09: save_blocker = "" when the game can be saved now, else why not (asked when the Save dialog opens);
    // save_exists = a save of this name exists (Save then asks to overwrite). Null: always allowed / never asks.
    std::function<std::u32string()> save_blocker;
    std::function<bool(const std::string&)> save_exists;
    std::function<std::vector<SkirmishMapInfo>()> skirmish_maps;
    // p4uia t01: the 9 GameConstants MP_Color_* entries (the host palette) the skirmish colour combos offer; null / empty:
    // the combos stay hidden.
    std::function<std::vector<Rgba8>()> player_colors;
    // p4uim t03: the preview of a map row (null: no image). Called when the selection changes; the host caches.
    std::function<SkirmishPreview(const SkirmishMapInfo&)> skirmish_preview;
    std::function<AudioVolumes()> audio;
    std::function<VideoSettings()> video;
    std::function<GameplaySettings()> gameplay;
    GameOptionsSetup game_option_defaults;  // ui1 t06: the advanced options' defaults and slider range (host: GameConstants)
    std::function<AdvancedVideoSettings()> advanced_video;  // null: defaults
    std::function<CameraSettings()> camera_settings;        // [port: Gabriel] null: defaults
    HotkeyTable* hotkeys = nullptr;  // edited by the keyboard dialogs (Accept)
    Bindings* bindings = nullptr;    // synced with apply_to_bindings on Accept
    // modc t20: the skirmish staging faction combos. The host fills it from the data (alamo_game: the Is_Playable,
    // non-neutral factions in FactionFiles.xml order, each with its Text_ID); empty = the combos stay empty and the
    // StartSkirmish action names no faction (the host's default_skirmish_faction).
    std::vector<SkirmishFaction> skirmish_factions;
    // modc t08: the mod picker's rows; null = no Mods button on the main menu.
    std::function<std::vector<ModPickerEntry>()> mods;
    bool demo = false;               // show IDC_MAIN_MENU_PLAY_DEMO instead of Single Player
};

// Keyboard dialogs: IDC_EDIT_* control -> hotkey slot (-1 when the port table has no entry).
int32_t keyboard_edit_slot(std::string_view control_id);
// "Ctrl+A", "F12", "Numpad -", "" for unbound.
std::u32string hotkey_label(const HotkeyBinding& b);
// UI5 t05 [port]: an English name for a hotkey slot (its hotkeys.cpp name with "Ability_" / "FoC" dropped and spaces
// between words: "Ability_SpreadOut" -> "Spread Out", "Taunt1" -> "Taunt 1"), for the "already in use" question.
std::u32string hotkey_action_label(uint16_t slot);

class FrontEnd {
public:
    void init(GuiManager& gui, CampaignMenu menu, MenuProviders providers);
    void show_main_menu();
    void show_pause_menu();       // in-game menu (IDD_GAME_OPTIONS_DIALOG) over the HUD
    void close_pause_menu();
    bool pause_menu_open() const;
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
    // p4ui t06: the row of `map` (its SkirmishMapInfo::map, case-insensitive) in the skirmish staging list as shown now
    // (active tab, sorted), -1 when that map is not listed there. Valid while the skirmish dialog is open.
    int32_t skirmish_map_row(std::string_view map) const;
    // Which tab the skirmish staging list shows: false Official (the dialog's state when it opens), true Custom.
    bool skirmish_custom_tab() const { return custom_tab_; }
    // p4uim t01: the game type IDC_GAME_TYPE_LIST selects (the list shows the maps of that type): true Space, false Land
    // or Capture Control Points. A dialog opened for the first time starts on Land; later on the type selected last,
    // remembered at once (UI.md 9.7).
    bool skirmish_space_type() const { return game_type_ == kSkirmishGameSpace; }
    // ccpgt t02: the selected game type (kSkirmishGameLand / Space / Control) and the IDC_GAME_TYPE_LIST row of `type`
    // (the rows are sorted by their text, LBS_SORT, UI.md 9.7), -1 when not listed.
    int32_t skirmish_game_type() const { return game_type_; }
    int32_t skirmish_game_type_row(int32_t type) const;
    // p4uia t01: the colour index shown in skirmish slot `slot` (0..7), -1 when the slot is not shown.
    int32_t skirmish_colour(int32_t slot) const;
    // modc t08: the load report of picker row `id` (lines as the host formats them). Kept per id; shown when that row is
    // selected, and Launch is enabled once the selected row has one (never for the active row).
    void set_mod_report(std::string_view id, std::vector<std::u32string> lines);
    // The picker row selected now ("" = the base game or no picker open).
    const std::string& selected_mod() const { return mod_selected_; }

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
    void fill_skirmish_slots(GuiDialog& d);  // ui1 t07: the player rows for the selected map's player count
    void fill_game_options(GuiDialog& d, const GameOptionsSetup& o);  // ui1 t06
    GameOptionsSetup read_game_options(const GuiDialog& d) const;
    void show_game_option_values(GuiDialog& d) const;
    void fill_skirmish_maps(GuiDialog& d);  // the list of the active tab and game type, from maps_
    void fill_skirmish_game_types(GuiDialog& d);
    void fill_skirmish_colours(GuiDialog& d);
    void fill_skirmish_preview(GuiDialog& d);  // IDC_IMAGE and its start numbers; IDC_SCENARIO_DESCRIPTION_TEXT_BOX stays hidden
    void resolve_skirmish_colours(GuiDialog& d);  // the colour clash rule (UI.md 9.7), slot order
    std::u32string skirmish_row_text(const SkirmishMapInfo& m) const;
    void fill_saves(GuiDialog& d);
    void fill_audio(GuiDialog& d, const AudioVolumes& v);
    void fill_video(GuiDialog& d);
    void fill_advanced_video(GuiDialog& d, const AdvancedVideoSettings& s);
    AdvancedVideoSettings read_advanced_video(const GuiDialog& d) const;
    void fill_port_controls(GuiDialog& d, const CameraSettings& s);
    CameraSettings read_port_controls(const GuiDialog& d) const;
    void fill_gameplay(GuiDialog& d, const GameplaySettings& g);
    void fill_keyboard(GuiDialog& d);
    void fill_mods(GuiDialog& d);         // modc t08: rows from MenuProviders::mods, the selected row's report
    void show_mod_report(GuiDialog& d);   // the report list and Launch state of mod_selected_
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
    // UI5 t05: a key the capture field took that another slot already uses waits for the "already in use" answer.
    int32_t rebind_slot_ = -1, rebind_other_ = -1;
    HotkeyBinding rebind_key_{};
    void apply_keys(std::vector<MenuAction>& out);
    void refill_keys();
    std::vector<SaveGameInfo> saves_;
    // [port: Gabriel] the saved games tree: one entry per list row, a save (index into saves_) or a group (save = -1, its
    // key path); the open groups' key paths (kept across refills).
    struct SaveRow {
        int32_t save = -1;
        std::string group;
    };
    std::vector<SaveRow> save_rows_;
    std::vector<std::string> saves_open_;
    void build_save_rows(GuiDialog& d, int32_t selected, int32_t top);
    int32_t row_save(int32_t row) const;               // a list row's save (index into saves_), -1 for a group / no row
    int32_t selected_save(const GuiDialog& d) const;  // the selected row's save (index into saves_), -1 for none / a group
    bool toggle_save_group(GuiDialog& d, int32_t row);  // a group row: open / close it (false: not a group row)
    std::vector<SkirmishMapInfo> maps_;
    std::vector<size_t> map_view_;   // skirmish list rows -> index into maps_ (active tab, sorted)
    bool custom_tab_ = false;        // skirmish list: Custom tab active
    int32_t game_type_ = kSkirmishGameLand;
    int32_t skirmish_slots_ = 2;  // ui1 t07: player rows shown (the selected map's players, 2..8)
    std::vector<std::u32string> skirmish_factions_;  // ui1 t07: the faction combo rows
    GameOptionsSetup game_options_;  // ui1 t06: the advanced options accepted last (set = false until then)  // skirmish list: the game type selected last (UI.md 9.7)
    std::vector<int32_t> game_type_rows_;    // IDC_GAME_TYPE_LIST rows -> game type (sorted by row text)
    // The map selected last [0 land, 1 space] ("" = none yet; UI.md 9.5 / 9.7: retail keeps one land key for types 2 and 4).
    std::string last_map_[2];
    std::vector<Rgba8> colour_palette_;   // the host palette read when the skirmish dialog opened
    int32_t slot_colour_[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // colour index of each skirmish slot (default = the slot index)
    std::string quit_action_;  // "exit" / "main"; N1 / t09: "overwrite" / "delete" (confirm_save_); UI5 t05: "rebind" / "keys"
    std::string confirm_save_;  // the save an overwrite / delete confirmation is about
    bool save_blocked_ = false; // the open Save dialog's save_blocker refused
    int32_t vsync_on_row_ = 1;  // U9: the V-sync combo row that re-checking IDC_VSYNC_ENABLED returns to
    bool video_status_stale_ = false;  // an ApplyVideo went out: re-read IDC_PORT_DISPLAY_INFO once the host has handled it
    bool res_confirm_ = false;  // t12: the confirmation dialog is open
    double res_deadline_ = 0.0, now_ = 0.0;
    int32_t res_shown_ = -1;    // seconds last written into the countdown line
    VideoSettings res_keep_, res_revert_;
    std::vector<ModPickerEntry> mods_;  // modc t08: the picker's rows as opened
    std::string mod_selected_;
    bool mod_chosen_ = false;  // mod_selected_ names a row (set by the first fill; the base row's id is "")
    std::vector<std::pair<std::string, std::vector<std::u32string>>> mod_reports_;  // per id, in arrival order
};

}  // namespace alamo::ui
