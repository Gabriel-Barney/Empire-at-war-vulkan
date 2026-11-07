#include "alamo/ui/gui_menus.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>

#include "alamo/core/ascii.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/ui/hud_components.hpp"  // utf8_to_utf32
#include "ui_strings.hpp"

namespace alamo::ui {

namespace {

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// UI.md 9.4: a map without a name key lists as its file name, directory and ".TED" dropped, cut to 22 characters + "..." when
// longer than 25.
std::string map_file_label(std::string_view map) {
    if (const size_t s = map.find_last_of("\\/"); s != std::string_view::npos) map.remove_prefix(s + 1);
    if (map.size() >= 4 && ascii_iequals(map.substr(map.size() - 4), ".TED")) map.remove_suffix(4);
    if (map.size() > 25) return std::string(map.substr(0, 22)) + "...";
    return std::string(map);
}

// Case-insensitive row order (ASCII folded; other code points by value): the original's _stricmp.
char32_t fold32(char32_t c) { return c >= U'A' && c <= U'Z' ? static_cast<char32_t>(c - U'A' + U'a') : c; }
bool row_text_less(const std::u32string& a, const std::u32string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const char32_t x = fold32(a[i]), y = fold32(b[i]);
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

constexpr const char* kMain = "IDD_MAIN_MENU_DIALOG";
constexpr const char* kSingle = "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG";
constexpr const char* kCampaignSelect = "IDD_CAMPAIGN_SELECTION_DIALOG";
constexpr const char* kCustomStaging = "IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG";
constexpr const char* kAdvanced = "IDD_CAMPAIGN_ADVANCED_OPTIONS_DIALOG";
constexpr const char* kSkirmish = "IDD_SKIRMISH_STAGING_DIALOG";
constexpr const char* kTutorial = "IDD_SELECT_TUTORIAL_DIALOG";
constexpr const char* kOptions = "IDD_MAIN_OPTIONS_DIALOG";
constexpr const char* kInGameOptions = "IDD_IN_GAME_OPTIONS_DIALOG";
constexpr const char* kAudio = "IDD_AUDIO_OPTIONS_DIALOG";
constexpr const char* kVideo = "IDD_VIDEO_OPTIONS_DIALOG";
constexpr const char* kAdvancedVideo = "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG";
// [port: Gabriel] each detail option the shipped dialog has as a 0..100 slider is a list of named levels (LevelChoice); the
// shipped slider and the Low / High captions at its ends are hidden, the port's list takes its place.
struct AdvLevel {
    const char* slider;
    const char* combo;
    int32_t AdvancedVideoSettings::*field;
    std::span<const LevelChoice> choices;
};
constexpr AdvLevel kAdvLevels[] = {
    {"IDC_PARTICLE_DETAIL_SLIDER", "IDC_PORT_PARTICLE_DETAIL", &AdvancedVideoSettings::particle, kLevels4},
    {"IDC_MESH_DETAIL_SLIDER", "IDC_PORT_MESH_DETAIL", &AdvancedVideoSettings::mesh, kLevels4},
    {"IDC_TEXTURE_MIP_DETAIL_SLIDER", "IDC_PORT_TEXTURE_DETAIL", &AdvancedVideoSettings::texture, kTextureChoices},
    {"IDC_WATER_DETAIL_SLIDER", "IDC_PORT_WATER_DETAIL", &AdvancedVideoSettings::water, kLevels3},
    {"IDC_SHADER_DETAIL_SLIDER", "IDC_PORT_SHADER_DETAIL", &AdvancedVideoSettings::shader, kLevels4},
    {"IDC_SHADOW_DETAIL_SLIDER", "IDC_PORT_SHADOW_DETAIL", &AdvancedVideoSettings::shadow, kShadowChoices},
    {"IDC_ENVIRONMENT_DETAIL_SLIDER", "IDC_PORT_ENVIRONMENT_DETAIL", &AdvancedVideoSettings::environment, kLevels4},
};
constexpr const char32_t* kQualityNames[kQualityCount + 1] = {U"Low", U"Medium", U"High", U"Ultra", U"Custom - see Advanced"};
struct AdvCheck {
    const char* id;
    bool AdvancedVideoSettings::*field;
};
constexpr AdvCheck kAdvChecks[] = {
    {"IDC_SOFT_SHADOWS", &AdvancedVideoSettings::soft_shadows},
    {"IDC_HEAT_DISTORTIONS", &AdvancedVideoSettings::heat_distortion},
    {"IDC_BLOOM", &AdvancedVideoSettings::bloom},
};
constexpr const char* kGameplay = "IDD_GAMEPLAY_OPTIONS_DIALOG";
constexpr float kAdvCheckWidth = 215.0f;  // QA D8 [port]: x 33 .. 248, clear of the combo column at x 297
constexpr const char* kPause = "IDD_GAME_OPTIONS_DIALOG";
constexpr const char* kSave = "IDD_SAVE_GAME_DIALOG";
constexpr const char* kLoad = "IDD_LOAD_GAME_DIALOG";
constexpr const char* kQuit = "IDD_DIALOG_BOX_QUIT_CONFIRM";
constexpr const char* kResConfirm = "IDD_DIALOG_BOX2_CENTERED";  // t12: wide two-button box with three centred text lines
constexpr const char* kKeyboardPages[5] = {"IDD_KEYBOARD_OPTIONS_DIALOG", "IDD_KEYBOARD_OPTIONS_DIALOG_2",
                                           "IDD_KEYBOARD_OPTIONS_DIALOG_3", "IDD_KEYBOARD_OPTIONS_DIALOG_4",
                                           "IDD_KEYBOARD_OPTIONS_DIALOG_5"};
constexpr const char* kDifficultyKeys[3] = {"TEXT_DIFFICULTY_EASY_NAME", "TEXT_DIFFICULTY_NORMAL_NAME",
                                            "TEXT_DIFFICULTY_HARD_NAME"};

bool is_port_controls(std::string_view dialog) { return ascii_iequals(dialog, kPortControlsDialog); }

int32_t keyboard_page(std::string_view dialog) {
    for (int32_t i = 0; i < 5; ++i)
        if (ascii_iequals(dialog, kKeyboardPages[i])) return i;
    return -1;
}

float to01(int32_t v) { return static_cast<float>(std::clamp(v, 0, 100)) / 100.0f; }
int32_t to100(float v) { return static_cast<int32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 100.0f)); }

// [port] IDC_EDIT_* (keyboard dialogs) -> hotkey slot name (hotkeys.hpp). Ability rows matched by meaning (guesses).
struct EditSlot {
    const char* edit;
    const char* slot;
};
constexpr EditSlot kEditSlots[] = {
    {"IDC_EDIT_SELECT_ALL", "SelectAll"}, {"IDC_EDIT_SELECT_LIKE", "SelectLike"}, {"IDC_EDIT_NEXT_UNIT", "NextUnit"},
    {"IDC_EDIT_PREV_UNIT", "PreviousUnit"}, {"IDC_EDIT_GUARD", "Guard"}, {"IDC_EDIT_ATTACK", "Attack"},
    {"IDC_EDIT_STOP", "Stop"}, {"IDC_EDIT_MOVE", "Move"}, {"IDC_EDIT_WAYPOINT", "Waypoint"}, {"IDC_EDIT_RETREAT", "Retreat"},
    {"IDC_EDIT_REINFORCE", "Reinforce"}, {"IDC_EDIT_PLACE_BEACON", "PlaceBeacon"}, {"IDC_EDIT_SUPER_WEAPON", "SuperWeapon"},
    {"IDC_EDIT_ATTACK_MOVE", "AttackMove"}, {"IDC_EDIT_DEPLOY", "Ability_Deploy"}, {"IDC_EDIT_SPREAD_OUT", "Ability_SpreadOut"},
    {"IDC_EDIT_JET_PACK", "Ability_JetPack"}, {"IDC_EDIT_THERMAL_DETONATOR", "Ability_StickyBomb"},
    {"IDC_EDIT_THERMAL_DETONATOR_DROP", "Ability_UntargetedStickyBomb"}, {"IDC_EDIT_ROCKET_ATTACK", "Ability_RocketAttack"},
    {"IDC_EDIT_CABLE_ATTACK", "Ability_CableAttack"}, {"IDC_EDIT_SENSOR_PING", "Ability_FowRevealPing"},
    {"IDC_EDIT_BOOST_WEAPON_POWER", "Ability_PowerToWeapons"}, {"IDC_EDIT_SELF_DESTRUCT", "Ability_SelfDestruct"},
    {"IDC_EDIT_DEPLOY_STORMTROOPERS", "Ability_DeployTroopers"}, {"IDC_EDIT_MAXIMUM_FIREPOWER", "Ability_MaximumFirepower"},
    {"IDC_EDIT_SPRINT", "Ability_Sprint"}, {"IDC_EDIT_SYSTEM_CONTROL", "Ability_SystemControl"},
    {"IDC_EDIT_REMOTE_EXPLOSIVE", "Ability_PlaceRemoteBomb"}, {"IDC_EDIT_DETONATE_REMOTE", "Ability_DetonateRemoteBomb"},
    {"IDC_EDIT_MAGNEPULSE_SHOT", "Ability_FireLobbingSuperweapon"}, {"IDC_EDIT_LASER_DEFENSE", "Ability_LaserDefense"},
    {"IDC_EDIT_STIM_PACK", "Ability_StimPack"}, {"IDC_EDIT_SWAP_WEAPONS", "Ability_SwapWeapons"},
    {"IDC_EDIT_MINOR_LIFE_DRAIN", "Ability_MinorDrainLife"}, {"IDC_EDIT_PROXIMITY_MINES", "Ability_ProximityMines"},
    {"IDC_EDIT_DEFEND", "Ability_Defend"}, {"IDC_EDIT_TRACTOR_BEAM", "Ability_TractorBeam"},
    {"IDC_EDIT_GRAVITY_WELL", "Ability_Interdict"}, {"IDC_EDIT_BARRAGE_AREA", "Ability_Barrage"},
    {"IDC_EDIT_HUNT_ENEMIES", "Ability_Hunt"}, {"IDC_EDIT_BOOST_ENGINE_POWER", "Ability_Turbo"},
    {"IDC_EDIT_LURE_FIGHTERS", "Ability_Lure"}, {"IDC_EDIT_MISSILE_JAMMING_FIELD", "Ability_MissileShield"},
    {"IDC_EDIT_LOCK_WINGS", "Ability_SpoilerLock"}, {"IDC_EDIT_ION_CANON_SHOT", "Ability_IonCannonShot"},
    {"IDC_EDIT_DESTROY_PLANET", "Ability_DestroyPlanet"}, {"IDC_EDIT_SENSOR_JAMMING", "Ability_SensorJamming"},
    {"IDC_EDIT_STEALTH", "Ability_Stealth"}, {"IDC_EDIT_LEECH_SHIELDS", "Ability_LeechShields"},
    {"IDC_EDIT_BUZZ_DROIDS", "Ability_BuzzDroids"}, {"IDC_EDIT_SUPER_LASER", "Ability_SuperLaser"},
    {"IDC_EDIT_CLUSTER_BOMB", "Ability_ClusterBomb"}, {"IDC_EDIT_SPACE_LASER_DEFENSE", "Ability_SpaceLaserDefense"},
    {"IDC_EDIT_SPACE_SELF_DESTRUCT", "Ability_SpaceSelfDestruct"}, {"IDC_EDIT_FULL_SALVO", "Ability_FullSalvo"},
    {"IDC_EDIT_DEPLOY_SQUADRON", "DeploySquad"}, {"IDC_EDIT_DROID_ADVISOR", "DroidAdvisor"},
    {"IDC_EDIT_INITIATE_CHAT", "InitiateChat"}, {"IDC_EDIT_SCROLL_UP", "ScrollUp"}, {"IDC_EDIT_SCROLL_DOWN", "ScrollDown"},
    {"IDC_EDIT_SCROLL_LEFT", "ScrollLeft"}, {"IDC_EDIT_SCROLL_RIGHT", "ScrollRight"}, {"IDC_EDIT_CAMERA_RESET", "CameraReset"},
    {"IDC_EDIT_CAMERA_CENTER", "CameraCentre"}, {"IDC_EDIT_CAMERA_HOME", "CameraHome"},
    {"IDC_EDIT_TACTICAL_OVERVIEW", "TacticalOverview"}, {"IDC_EDIT_TETHER_CAMERA", "CameraTether"},
    {"IDC_EDIT_BATTLE_CAM", "MovieMode"}, {"IDC_EDIT_CINEMATIC_PREVIEW", "CinematicView"},
    {"IDC_EDIT_SCREEN_SHOT", "Screenshot"}, {"IDC_EDIT_QUICK_SAVE", "QuickSave"}, {"IDC_EDIT_QUICK_LOAD", "QuickLoad"},
    {"IDC_EDIT_MISSION_HOLOCRON", "MissionHolocron"}, {"IDC_EDIT_TAUNT_ONE", "Taunt1"}, {"IDC_EDIT_TAUNT_TWO", "Taunt2"},
    {"IDC_EDIT_TAUNT_THREE", "Taunt3"}, {"IDC_EDIT_GIVE_CREDITS", "GiveCredits"},
    {"IDC_EDIT_TOGGLE_INTERFACE", "ToggleInterface"}, {"IDC_EDIT_TOGGLE_PLAYER_LIST", "TogglePlayerList"},
    {"IDC_EDIT_LUCKY_SHOT", "Ability_LuckyShot"}, {"IDC_EDIT_CALL_WINGMEN", "Ability_ReplenishWingmen"},
    {"IDC_EDIT_FORCE_CRUSH", "Ability_Telekinesis"}, {"IDC_EDIT_FORCE_PUSH", "Ability_Whirlwind"},
    {"IDC_EDIT_FORCE_LIGHTNING", "Ability_Lightning"}, {"IDC_EDIT_FORCE_CORRUPT", "Ability_AreaConvert"},
    {"IDC_EDIT_FORCE_PROTECT", "Ability_TargetedInvulnerability"}, {"IDC_EDIT_CAPTURE_VEHICLE", "Ability_CaptureVehicle"},
    {"IDC_EDIT_EJECT_VEHICLE_THIEF", "Ability_EjectVehicleThief"}, {"IDC_EDIT_PROTON_BEAM", "Ability_EnergyWeapon"},
    {"IDC_EDIT_SEISMIC_CHARGE", "Ability_HarmonicBomb"}, {"IDC_EDIT_WEAKEN", "Ability_WeakenEnemy"},
    {"IDC_EDIT_INVULNERABILITY", "Ability_Invulnerability"}, {"IDC_EDIT_AOE_STUN", "Ability_AreaStun"},
    {"IDC_EDIT_COMMAND_TURRET", "Ability_TargetedHack"}, {"IDC_EDIT_REPAIR_VEHICLE", "Ability_TargetedRepair"},
    {"IDC_EDIT_FLAME_THROWER", "Ability_FlameThrower"}, {"IDC_EDIT_REDIRECT_FIREPOWER", "Ability_HomeOne"},
    {"IDC_EDIT_LIGHTSABER_THROW", "Ability_SaberThrow"}, {"IDC_EDIT_HERO_CLOAK", "Ability_ForceCloak"},
    {"IDC_EDIT_FORCE_CONFUSE", "Ability_ForceConfuse"}, {"IDC_EDIT_FORCE_SIGHT", "Ability_ForceSight"},
    {"IDC_EDIT_BERSERKER", "Ability_Berserker"}, {"IDC_EDIT_CONTAMINATE", "Ability_RadioactiveContaminate"},
    {"IDC_EDIT_INFECTION", "Ability_Infection"}, {"IDC_EDIT_DRAIN_LIFE", "Ability_DrainLife"},
    {"IDC_EDIT_SUMMON_RANCOR", "Ability_Summon"}, {"IDC_EDIT_BLAST", "Ability_Blast"},
    {"IDC_EDIT_SHIELD_FLARE", "Ability_ShieldFlare"}, {"IDC_EDIT_SONIC_STUN", "Ability_Stun"},
    {"IDC_EDIT_BRIBE", "Ability_TacticalBribe"}, {"IDC_EDIT_ASSAULT", "Ability_FoCFullSalvo"},
    {"IDC_EDIT_SENSOR_JAM", "Ability_FoCSensorJamming"},
    // [port: Gabriel] IDD_PORT_CAMERA_CONTROLS_DIALOG
    {"IDC_EDIT_PORT_PAUSE", "Pause"}, {"IDC_EDIT_PORT_YAW_LEFT", "CameraYawLeft"}, {"IDC_EDIT_PORT_YAW_RIGHT", "CameraYawRight"},
    {"IDC_EDIT_PORT_PITCH_UP", "CameraPitchUp"}, {"IDC_EDIT_PORT_PITCH_DOWN", "CameraPitchDown"},
    {"IDC_EDIT_PORT_ROLL_LEFT", "CameraRollLeft"}, {"IDC_EDIT_PORT_ROLL_RIGHT", "CameraRollRight"},
    {"IDC_EDIT_PORT_SCROLL_UP", "ScrollUp"}, {"IDC_EDIT_PORT_SCROLL_DOWN", "ScrollDown"}, {"IDC_EDIT_PORT_SCROLL_LEFT", "ScrollLeft"},
    {"IDC_EDIT_PORT_SCROLL_RIGHT", "ScrollRight"}, {"IDC_EDIT_PORT_ZOOM_IN", "CameraZoomIn"},
    {"IDC_EDIT_PORT_ZOOM_OUT", "CameraZoomOut"},
};

MenuAction act(MenuAction::Kind k) {
    MenuAction a;
    a.kind = k;
    return a;
}

}  // namespace

const char* menu_action_name(MenuAction::Kind k) {
    switch (k) {
        case MenuAction::Kind::StartCampaign: return "StartCampaign";
        case MenuAction::Kind::StartSkirmish: return "StartSkirmish";
        case MenuAction::Kind::StartTutorial: return "StartTutorial";
        case MenuAction::Kind::LoadGame: return "LoadGame";
        case MenuAction::Kind::SaveGame: return "SaveGame";
        case MenuAction::Kind::DeleteSave: return "DeleteSave";
        case MenuAction::Kind::ResumeGame: return "ResumeGame";
        case MenuAction::Kind::QuitToMainMenu: return "QuitToMainMenu";
        case MenuAction::Kind::ExitGame: return "ExitGame";
        case MenuAction::Kind::OpenMultiplayer: return "OpenMultiplayer";
        case MenuAction::Kind::ShowCredits: return "ShowCredits";
        case MenuAction::Kind::PreviewAudio: return "PreviewAudio";
        case MenuAction::Kind::ApplyAudio: return "ApplyAudio";
        case MenuAction::Kind::ApplyVideo: return "ApplyVideo";
        case MenuAction::Kind::ApplyGameplay: return "ApplyGameplay";
        case MenuAction::Kind::KeybindingsChanged: return "KeybindingsChanged";
        case MenuAction::Kind::ApplyAdvancedVideo: return "ApplyAdvancedVideo";
        case MenuAction::Kind::AutoDetectVideo: return "AutoDetectVideo";
        case MenuAction::Kind::ApplyCamera: return "ApplyCamera";
        case MenuAction::Kind::CheckMod: return "CheckMod";
        case MenuAction::Kind::LaunchMod: return "LaunchMod";
    }
    return "?";
}

// [port: Gabriel] the port's camera / controls dialog, in the GUIDialogs.rc syntax (skinned by the Default skin).
const char* const kPortDialogsRc = R"RC(
IDD_PORT_CAMERA_CONTROLS_DIALOG DIALOGEX 0, 0, 560, 440
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LTEXT           "Camera and Controls",IDC_STATIC_MEDIUM,17,10,483,17
    RTEXT           "Galactic Field of View",IDC_PORT_GALACTIC_FOV_TEXT,16,58,200,18
    CONTROL         "",IDC_PORT_GALACTIC_FOV_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,226,58,240,18
    LTEXT           "27",IDC_PORT_GALACTIC_FOV_VALUE,476,58,60,18
    RTEXT           "Tactical Field of View",IDC_PORT_TACTICAL_FOV_TEXT,16,86,200,18
    CONTROL         "",IDC_PORT_TACTICAL_FOV_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,226,86,240,18
    LTEXT           "55",IDC_PORT_TACTICAL_FOV_VALUE,476,86,60,18
    RTEXT           "Pause",IDC_STATIC,16,122,200,18
    EDITTEXT        IDC_EDIT_PORT_PAUSE,226,122,150,18,ES_AUTOHSCROLL
    RTEXT           "Scroll Up / Down",IDC_STATIC,16,146,200,18
    EDITTEXT        IDC_EDIT_PORT_SCROLL_UP,226,146,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_SCROLL_DOWN,386,146,150,18,ES_AUTOHSCROLL
    RTEXT           "Scroll Left / Right",IDC_STATIC,16,170,200,18
    EDITTEXT        IDC_EDIT_PORT_SCROLL_LEFT,226,170,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_SCROLL_RIGHT,386,170,150,18,ES_AUTOHSCROLL
    RTEXT           "Rotate Left / Right",IDC_STATIC,16,194,200,18
    EDITTEXT        IDC_EDIT_PORT_YAW_LEFT,226,194,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_YAW_RIGHT,386,194,150,18,ES_AUTOHSCROLL
    RTEXT           "Pitch Up / Down",IDC_STATIC,16,218,200,18
    EDITTEXT        IDC_EDIT_PORT_PITCH_UP,226,218,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_PITCH_DOWN,386,218,150,18,ES_AUTOHSCROLL
    RTEXT           "Roll Left / Right",IDC_STATIC,16,242,200,18
    EDITTEXT        IDC_EDIT_PORT_ROLL_LEFT,226,242,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_ROLL_RIGHT,386,242,150,18,ES_AUTOHSCROLL
    RTEXT           "Zoom In / Out",IDC_STATIC,16,266,200,18
    EDITTEXT        IDC_EDIT_PORT_ZOOM_IN,226,266,150,18,ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_PORT_ZOOM_OUT,386,266,150,18,ES_AUTOHSCROLL
    RTEXT           "Queue Orders With",IDC_STATIC,16,302,200,18
    COMBOBOX        IDC_PORT_QUEUE_MODIFIER,226,302,150,80,CBS_DROPDOWNLIST | WS_TABSTOP
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_CANCEL_BUTTON,14,398,160,24
    PUSHBUTTON      "TEXT_BUTTON_DEFAULTS",IDC_DEFAULTS_BUTTON,200,398,160,24
    PUSHBUTTON      "TEXT_BUTTON_ACCEPT",IDC_BUTTON_ACCEPT,386,398,160,24
END
IDD_PORT_MODS_DIALOG DIALOGEX 0, 0, 760, 440
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LTEXT           "Mods",IDC_STATIC_MEDIUM,17,10,483,17
    LTEXT           "Installed",IDC_PORT_MODS_LIST_TEXT,16,40,320,16
    LISTBOX         IDC_PORT_MODS_LIST,16,58,320,326,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LTEXT           "Load report",IDC_PORT_MODS_REPORT_TEXT,350,40,394,16
    LISTBOX         IDC_PORT_MODS_REPORT,350,58,394,326,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    PUSHBUTTON      "TEXT_BUTTON_BACK",IDC_CANCEL_BUTTON,14,398,160,24
    PUSHBUTTON      "Launch",IDC_PORT_MODS_LAUNCH,586,398,160,24
END
)RC";

const char* const kPortDialogExtensionsRc = R"RC(
IDD_MAIN_MENU_DIALOG DIALOGEX 0, 0, 512, 1024
BEGIN
    PUSHBUTTON      "Mods",IDC_PORT_MAIN_MENU_MODS,265,742,236,22
END
IDD_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 570, 454
BEGIN
    LTEXT           "V-Sync Mode",IDC_PORT_VSYNC_MODE_TEXT,357,142,188,18
    COMBOBOX        IDC_PORT_VSYNC_MODE,357,162,188,80,CBS_DROPDOWNLIST | WS_TABSTOP
    LTEXT           "Frame Rate Cap",IDC_PORT_FRAME_CAP_TEXT,357,188,188,18
    COMBOBOX        IDC_PORT_FRAME_CAP,357,208,188,90,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_ANTI_ALIASING,196,246,311,108,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_DETAIL_LEVEL,196,271,311,130,CBS_DROPDOWNLIST | WS_TABSTOP
    RTEXT           "",IDC_PORT_GAMMA_VALUE,143,297,32,21
    LTEXT           "",IDC_PORT_DISPLAY_INFO,22,376,526,18
END
IDD_ADVANCED_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 571, 454
BEGIN
    COMBOBOX        IDC_PORT_PARTICLE_DETAIL,230,75,251,108,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_MESH_DETAIL,230,105,251,108,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_WATER_DETAIL,230,135,251,88,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_SHADER_DETAIL,230,165,251,108,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_TEXTURE_DETAIL,230,195,251,88,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_SHADOW_DETAIL,230,225,251,66,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PORT_ENVIRONMENT_DETAIL,230,255,251,108,CBS_DROPDOWNLIST | WS_TABSTOP
    LTEXT           "Anisotropic Filtering",IDC_PORT_ANISOTROPY_TEXT,297,360,218,18
    COMBOBOX        IDC_PORT_ANISOTROPY,297,380,218,120,CBS_DROPDOWNLIST | WS_TABSTOP
    CONTROL         "Ambient Occlusion",IDC_PORT_AMBIENT_OCCLUSION,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,33,385,215,23
END
)RC";

namespace {
constexpr uint8_t kQueueModChoices[4] = {kModShift, kModCtrl, kModAlt, 0};
constexpr const char32_t* kQueueModNames[4] = {U"Shift", U"Ctrl", U"Alt", U"None"};
std::u32string fov_text(float deg) { return ascii32(std::to_string(static_cast<int>(std::lround(deg)))); }
std::u32string value_text(int32_t v) { return ascii32(std::to_string(v)); }
// QA D8 [port]: the combos the port adds hold plain ASCII choices, so they read in the dialog's caption font. The shipped
// combos keep the skin's Combo_Box font (GUIDialogs.xml: Arial Unicode MS, for translated text).
void use_ui_font(GuiDialog& d, std::initializer_list<const char*> ids) {
    for (const char* id : ids)
        if (GuiControl* c = d.get(id)) c->ui_font = true;
}
// [port: Gabriel] hides a shipped slider and the end captions beside it (Low / Off / Max / High: statics with no id of their
// own, found by their row and by sitting clear of the row's title on the left, which ends well before the slider's start).
void hide_slider(GuiDialog& d, const char* id) {
    const GuiControl* s = d.get(id);
    if (!s || !s->def) return;
    const int32_t x = s->def->x, y = s->def->y;
    d.show(id, false);
    for (size_t i = 0; i < d.size(); ++i) {
        GuiControl& c = d[i];
        if (c.type == GuiGadgetType::Text && c.def && c.def->x >= x - 70 && std::abs(c.def->y - y) <= 4) c.visible = false;
    }
}
// The items of a list of named levels, in the dialog's text path (pseudo-localization, translation).
std::vector<std::u32string> level_items(const GuiManager& gui, std::span<const LevelChoice> choices) {
    std::vector<std::u32string> items;
    for (const LevelChoice& c : choices) items.push_back(gui.port_text(c.label));
    return items;
}
// The advanced options the player is on now: the host's, else (a host with no advanced provider) the Detail value for all seven.
AdvancedVideoSettings current_advanced(const MenuProviders& p, const VideoSettings& v) {
    if (p.advanced_video) return p.advanced_video();
    AdvancedVideoSettings a;
    set_advanced_detail(a, v.detail);
    return a;
}
// The row a list has selected, when it names one of `count` choices.
std::optional<size_t> picked(const GuiDialog& d, const char* id, size_t count) {
    const GuiControl* c = d.get(id);
    if (!c || c->selected < 0 || static_cast<size_t>(c->selected) >= count) return std::nullopt;
    return static_cast<size_t>(c->selected);
}
// QA U11: the shipped IDD_GAME_OPTIONS_DIALOG leaves one button row empty between Options and Quit Game, with no control
// for it in GUIDialogs.rc. Quit and Resume move up by that row and the dialog ends that much sooner [port].
float pause_menu_gap(const GuiDialog& d) {
    const GuiControl* load = d.get("IDC_LOAD_GAME");
    const GuiControl* options = d.get("IDC_INGAME_OPTIONS");
    const GuiControl* quit = d.get("IDC_GAME_OPTIONS_QUIT");
    if (!load || !options || !quit) return 0.0f;
    return static_cast<float>(std::max(0, quit->def->y - options->def->y - (options->def->y - load->def->y)));
}
}  // namespace

void FrontEnd::fill_port_controls(GuiDialog& d, const CameraSettings& s) {
    d.set_range("IDC_PORT_GALACTIC_FOV_SLIDER", static_cast<int32_t>(kGalacticFovMinDeg), static_cast<int32_t>(kGalacticFovMaxDeg));
    d.set_range("IDC_PORT_TACTICAL_FOV_SLIDER", static_cast<int32_t>(kTacticalFovMinDeg), static_cast<int32_t>(kTacticalFovMaxDeg));
    d.set_value("IDC_PORT_GALACTIC_FOV_SLIDER", static_cast<int32_t>(std::lround(s.galactic_fov_deg)));
    d.set_value("IDC_PORT_TACTICAL_FOV_SLIDER", static_cast<int32_t>(std::lround(s.tactical_fov_deg)));
    d.set_text("IDC_PORT_GALACTIC_FOV_VALUE", fov_text(s.galactic_fov_deg));
    d.set_text("IDC_PORT_TACTICAL_FOV_VALUE", fov_text(s.tactical_fov_deg));
    std::vector<std::u32string> mods;
    for (const char32_t* m : kQueueModNames) mods.push_back(gui_->port_text(m));
    int32_t sel = 0;
    for (int32_t i = 0; i < 4; ++i)
        if (kQueueModChoices[i] == s.queue_mods) sel = i;
    d.set_items("IDC_PORT_QUEUE_MODIFIER", std::move(mods), sel);
    use_ui_font(d, {"IDC_PORT_QUEUE_MODIFIER"});
    fill_keyboard(d);
}

CameraSettings FrontEnd::read_port_controls(const GuiDialog& d) const {
    CameraSettings s = p_.camera_settings ? p_.camera_settings() : CameraSettings{};
    if (const GuiControl* c = d.get("IDC_PORT_GALACTIC_FOV_SLIDER")) s.galactic_fov_deg = static_cast<float>(c->value);
    if (const GuiControl* c = d.get("IDC_PORT_TACTICAL_FOV_SLIDER")) s.tactical_fov_deg = static_cast<float>(c->value);
    if (const GuiControl* c = d.get("IDC_PORT_QUEUE_MODIFIER"); c && c->selected >= 0 && c->selected < 4)
        s.queue_mods = kQueueModChoices[c->selected];
    return s;
}

int32_t keyboard_edit_slot(std::string_view control_id) {
    if (ascii_istarts_with(control_id, "IDC_EDIT_GROUP_") && control_id.size() == std::string_view("IDC_EDIT_GROUP_0_SELECT").size()) {
        const char d = control_id[15];
        if (d >= '0' && d <= '9') return d == '0' ? 9 : d - '1';  // slot 0..8 = groups 1..9, slot 9 = group 0
    }
    for (const EditSlot& e : kEditSlots)
        if (ascii_iequals(control_id, e.edit)) return hotkey_slot(e.slot);
    return -1;
}

std::u32string hotkey_label(const HotkeyBinding& b) {
    if (b.key < 0) return {};
    std::string s;
    if (b.mod == HotkeyMod::Shift) s = "Shift+";
    if (b.mod == HotkeyMod::Ctrl) s = "Ctrl+";
    if (b.mod == HotkeyMod::Alt) s = "Alt+";
    const int32_t k = b.key;
    char buf[16];
    if ((k >= '0' && k <= '9') || (k >= 'A' && k <= 'Z')) {
        s.push_back(static_cast<char>(k));
    } else if (k >= 0x70 && k <= 0x87) {
        std::snprintf(buf, sizeof buf, "F%d", k - 0x6F);
        s += buf;
    } else if (k >= 0x60 && k <= 0x69) {
        std::snprintf(buf, sizeof buf, "Numpad %d", k - 0x60);
        s += buf;
    } else {
        switch (k) {
            case 0x08: s += "Backspace"; break;
            case 0x09: s += "Tab"; break;
            case 0x0D: s += "Enter"; break;
            case 0x13: s += "Pause"; break;
            case 0x1B: s += "Esc"; break;
            case 0x20: s += "Space"; break;
            case 0x21: s += "Page Up"; break;
            case 0x22: s += "Page Down"; break;
            case 0x23: s += "End"; break;
            case 0x24: s += "Home"; break;
            case 0x25: s += "Left"; break;
            case 0x26: s += "Up"; break;
            case 0x27: s += "Right"; break;
            case 0x28: s += "Down"; break;
            case 0x2D: s += "Insert"; break;
            case 0x2E: s += "Delete"; break;
            case 0x6A: s += "Numpad *"; break;
            case 0x6B: s += "Numpad +"; break;
            case 0x6D: s += "Numpad -"; break;
            case 0x6E: s += "Numpad ."; break;
            case 0x6F: s += "Numpad /"; break;
            case 0x91: s += "Scroll Lock"; break;
            case 0xBA: s += ";"; break;
            case 0xBB: s += "="; break;
            case 0xBC: s += ","; break;
            case 0xBD: s += "-"; break;
            case 0xBE: s += "."; break;
            case 0xBF: s += "/"; break;
            case 0xC0: s += "`"; break;
            case 0xDB: s += "["; break;
            case 0xDC: s += "\\"; break;
            case 0xDD: s += "]"; break;
            case 0xDE: s += "'"; break;
            default:
                std::snprintf(buf, sizeof buf, "Key %d", k);
                s += buf;
        }
    }
    return ascii32(s);
}

// ---- Campaign menu -------------------------------------------------------------------------------------------
CampaignMenu build_campaign_menu(std::span<const data::CampaignDef* const> sorted) {
    CampaignMenu m;
    auto add = [](std::vector<CampaignSetInfo>& list, const data::CampaignDef& c, const std::string& set,
                  const std::string& faction, bool custom) {
        CampaignSetInfo* s = nullptr;
        for (CampaignSetInfo& e : list)
            if (ascii_iequals(e.set, set)) s = &e;
        if (!s) {
            CampaignSetInfo n;
            n.set = set;
            n.text_id = trimmed(c.text_id);
            n.description_text = trimmed(c.description_text);
            n.sort_order = c.sort_order;
            n.custom = custom;
            list.push_back(std::move(n));
            s = &list.back();
        }
        for (const CampaignChoice& f : s->factions)
            if (ascii_iequals(f.faction, faction)) return;  // first campaign of a faction wins
        s->factions.push_back({c.name, faction});
    };
    for (const data::CampaignDef* c : sorted) {
        if (!c) continue;
        const std::string faction = trimmed(c->starting_active_player);
        if (faction.empty()) continue;  // multiplayer sets
        const std::string set = trimmed(c->campaign_set).empty() ? c->name : trimmed(c->campaign_set);
        const std::string_view cs = c->raw_value("Supports_Custom_Settings");
        const bool custom = ascii_iequals(trimmed(cs), "true") || ascii_iequals(trimmed(cs), "yes") || trimmed(cs) == "1";
        if (ascii_istarts_with(set, "Tutorial")) add(m.tutorials, *c, set, faction, custom);
        else add(custom ? m.custom : m.story, *c, set, faction, custom);
    }
    return m;
}

CampaignMenu build_campaign_menu(const data::GameDatabase& db) {
    const std::vector<const data::CampaignDef*> sorted = db.campaigns_by_sort_order();
    return build_campaign_menu(std::span<const data::CampaignDef* const>(sorted));
}

// ---- FrontEnd ------------------------------------------------------------------------------------------------
void FrontEnd::init(GuiManager& gui, CampaignMenu menu, MenuProviders providers) {
    gui_ = &gui;
    menu_ = std::move(menu);
    p_ = std::move(providers);
}

std::u32string FrontEnd::text(std::string_view key) const { return gui_->caption_text(key); }

std::u32string FrontEnd::text_or(std::string_view key, std::u32string fallback) const {
    if (gui_->text_db() && gui_->text_db()->find(key)) return gui_->caption_text(key);
    return gui_->port_text(fallback);
}

std::u32string FrontEnd::faction_label(std::string_view faction) const {
    const std::string key = "TEXT_FACTION_" + ascii_upper(faction);
    if (gui_->text_db() && gui_->text_db()->find(key)) return gui_->caption_text(key);
    return ascii32(faction);
}

std::string FrontEnd::top() const {
    if (!gui_ || gui_->open_count() == 0) return {};
    return gui_->open_names().back();
}

GuiDialog* FrontEnd::push(std::string_view dialog) {
    GuiDialog* d = gui_->open(dialog);
    if (d) setup(*d);
    return d;
}

void FrontEnd::go(std::string_view dialog) {
    const std::string t = top();
    if (!t.empty()) gui_->close(t);
    push(dialog);
}

void FrontEnd::pop() {
    const std::string t = top();
    if (!t.empty()) gui_->close(t);
}

void FrontEnd::show_main_menu() {
    in_game_ = false;
    gui_->close_all();
    gui_->set_backdrop("MenuBack_overlay.tga");  // [port] the front end's 2D backdrop
    push(kMain);
}

void FrontEnd::show_pause_menu() {
    in_game_ = true;
    gui_->set_backdrop({});
    if (!gui_->find(kPause)) push(kPause);
}

void FrontEnd::close_pause_menu() {
    // Everything opened from the pause menu sits above it.
    while (gui_->find(kPause)) pop();
}

bool FrontEnd::pause_menu_open() const { return gui_ && gui_->find(kPause) != nullptr; }

int32_t FrontEnd::difficulty_of(const GuiDialog& d) const {
    if (const GuiControl* s = d.get("IDC_CAMPAIGN_DIFFICULTY_SLIDER")) return s->value;
    for (int32_t i = 0; i < 3; ++i) {
        static constexpr const char* kRadios[3][2] = {{"IDC_RADIO_CUSTOM_EASY", "IDC_RADIO_EASY"},
                                                      {"IDC_RADIO_CUSTOM_MEDIUM", "IDC_RADIO_MEDIUM"},
                                                      {"IDC_RADIO_CUSTOM_HARD", "IDC_RADIO_HARD"}};
        for (const char* r : kRadios[i])
            if (const GuiControl* c = d.get(r); c && c->checked) return i;
    }
    return 1;
}

void FrontEnd::fill_campaign_select(GuiDialog& d) {
    std::vector<std::u32string> items;
    for (const CampaignSetInfo& s : menu_.story) items.push_back(text(s.text_id.empty() ? s.set : s.text_id));
    const int32_t sel = d.get("IDC_CAMPAIGNS_LIST") ? d.get("IDC_CAMPAIGNS_LIST")->selected : -1;
    d.set_items("IDC_CAMPAIGNS_LIST", std::move(items), sel >= 0 ? sel : (menu_.story.empty() ? -1 : 0));
    d.set_range("IDC_CAMPAIGN_DIFFICULTY_SLIDER", 0, 2);
    const int32_t diff = difficulty_of(d);
    d.set_text("IDC_STATIC_DIFFICULTY_NAME", text(kDifficultyKeys[std::clamp(diff, 0, 2)]));
    const GuiControl* list = d.get("IDC_CAMPAIGNS_LIST");
    const CampaignSetInfo* s = list && list->selected >= 0 && static_cast<size_t>(list->selected) < menu_.story.size()
                                   ? &menu_.story[static_cast<size_t>(list->selected)]
                                   : nullptr;
    const char* buttons[2] = {"IDC_PLAY_FACTION_A_BUTTON", "IDC_PLAY_FACTION_B_BUTTON"};
    for (size_t b = 0; b < 2; ++b) {
        const bool has = s && b < s->factions.size();
        d.show(buttons[b], has);
        if (has) d.set_text(buttons[b], faction_label(s->factions[b].faction));
    }
    d.set_text("IDC_STATIC_DIFFICULTY_DESCRIPTION", s ? text(s->description_text) : std::u32string());
}

void FrontEnd::fill_custom_staging(GuiDialog& d) {
    std::vector<std::u32string> items;
    for (const CampaignSetInfo& s : menu_.custom) items.push_back(text(s.text_id.empty() ? s.set : s.text_id));
    const GuiControl* l0 = d.get("IDC_STAGING_MAP_LIST");
    const int32_t sel = l0 && l0->selected >= 0 ? l0->selected : (menu_.custom.empty() ? -1 : 0);
    d.set_items("IDC_STAGING_MAP_LIST", std::move(items), sel);
    const CampaignSetInfo* s = sel >= 0 && static_cast<size_t>(sel) < menu_.custom.size() ? &menu_.custom[static_cast<size_t>(sel)]
                                                                                        : nullptr;
    std::vector<std::u32string> factions;
    if (s)
        for (const CampaignChoice& c : s->factions) factions.push_back(faction_label(c.faction));
    const GuiControl* fc = d.get("IDC_FACTION_COMBO");
    const int32_t fsel = fc && fc->selected >= 0 && static_cast<size_t>(fc->selected) < factions.size() ? fc->selected : 0;
    d.set_items("IDC_FACTION_COMBO", std::move(factions), fsel);
    d.set_text("IDC_CAMPAIGN_DESCRIPTION", s ? text(s->description_text) : std::u32string());
    d.show("IDC_SELECT_CUSTOM_MAPS", false);  // [port] no custom maps
}

std::u32string FrontEnd::skirmish_row_text(const SkirmishMapInfo& m) const {
    if (!m.label.empty()) return m.label;
    if (m.name_key.empty()) return ascii32(map_file_label(m.map));
    // "(%d) %s": the player count and the localised name; the raw key when the text database has no entry (UI.md 9.3).
    std::u32string t = gui_->text_db() && gui_->text_db()->find(m.name_key) ? gui_->caption_text(m.name_key) : utf8_to_utf32(m.name_key);
    return ascii32("(" + std::to_string(m.players ? m.players : m.max_players) + ") ") + t;
}

// The rows of the active tab and game type (UI.md 9.4: a space map is listed under Space, the others under Land), each by
// row text case-insensitively, ties by map id. The selection is the map selected last for that game type when it is listed
// (UI.md 9.5 / 9.7), else row 0 (as retail's 0xfb950; modc t19: no map is preferred by name). The row selected becomes the
// map remembered for the type.
void FrontEnd::fill_skirmish_maps(GuiDialog& d) {
    struct Row {
        size_t index;
        std::u32string text;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < maps_.size(); ++i)
        if (maps_[i].custom == custom_tab_ && maps_[i].space == space_type_) rows.push_back({i, skirmish_row_text(maps_[i])});
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const SkirmishMapInfo &ma = maps_[a.index], &mb = maps_[b.index];
        if (a.text != b.text) {
            const bool ab = row_text_less(a.text, b.text), ba = row_text_less(b.text, a.text);
            if (ab != ba) return ab;
        }
        if (ma.map != mb.map) return ma.map < mb.map;
        return a.index < b.index;
    });
    map_view_.clear();
    std::vector<std::u32string> items;
    int32_t sel = rows.empty() ? -1 : 0;
    int32_t last = -1;
    const std::string& last_map = last_map_[space_type_ ? 1 : 0];
    for (size_t r = 0; r < rows.size(); ++r) {
        const SkirmishMapInfo& m = maps_[rows[r].index];
        map_view_.push_back(rows[r].index);
        items.push_back(std::move(rows[r].text));
        if (last < 0 && !last_map.empty() && ascii_iequals(m.map, last_map)) last = static_cast<int32_t>(r);
    }
    if (last >= 0) sel = last;
    d.set_items("IDC_STAGING_MAP_LIST", std::move(items), sel);
    if (sel >= 0) last_map_[space_type_ ? 1 : 0] = maps_[map_view_[static_cast<size_t>(sel)]].map;
    fill_skirmish_preview(d);
}

int32_t FrontEnd::skirmish_map_row(std::string_view map) const {
    for (size_t r = 0; r < map_view_.size(); ++r)
        if (ascii_iequals(maps_[map_view_[r]].map, map)) return static_cast<int32_t>(r);
    return -1;
}

void FrontEnd::fill_skirmish_game_types(GuiDialog& d) {
    // Land first (game type 2), then Space (3) as in UI.md 9.4; the names are the text database's game type entries.
    d.set_items("IDC_GAME_TYPE_LIST", {text_or("TEXT_GAME_TYPE_TEAM_LAND", gui_->port_text(U"Land")),
                                       text_or("TEXT_GAME_TYPE_TEAM_SPACE", gui_->port_text(U"Space"))},
                space_type_ ? 1 : 0);
}

void FrontEnd::fill_skirmish_colours(GuiDialog& d) {
    char id[32];
    for (int32_t p = 0; p < 2; ++p) {  // the used slots [port: local player + one AI]
        std::snprintf(id, sizeof id, "IDC_PLAYER_COLOR_%d", p);
        const bool has = !colour_palette_.empty();
        d.show(id, has);
        if (!has) continue;
        // UI.md 3.7.1: one U+25A0 per palette entry, drawn in the entry's colour.
        d.set_items(id, std::vector<std::u32string>(colour_palette_.size(), std::u32string(1, 0x25A0)),
                    std::min(slot_colour_[p], static_cast<int32_t>(colour_palette_.size()) - 1));
        if (GuiControl* c = d.get(id)) c->item_colors = colour_palette_;
    }
}

int32_t FrontEnd::skirmish_colour(int32_t slot) const {
    return slot >= 0 && slot < 2 && !colour_palette_.empty() ? std::min(slot_colour_[slot], static_cast<int32_t>(colour_palette_.size()) - 1) : -1;
}

// IDC_IMAGE and its start numbers for the selected row (UI.md 9.6 / 9.6.1); IDC_SCENARIO_DESCRIPTION_TEXT_BOX is switched
// off and never filled (9.6.2).
void FrontEnd::fill_skirmish_preview(GuiDialog& d) {
    const GuiControl* list = d.get("IDC_STAGING_MAP_LIST");
    const SkirmishMapInfo* m = list && list->selected >= 0 && static_cast<size_t>(list->selected) < map_view_.size()
                                   ? &maps_[map_view_[static_cast<size_t>(list->selected)]]
                                   : nullptr;
    SkirmishPreview pv;
    if (m && p_.skirmish_preview) pv = p_.skirmish_preview(*m);
    d.set_image("IDC_IMAGE", pv.texture);
    if (GuiControl* c = d.get("IDC_IMAGE")) {
        c->markers.clear();
        for (size_t i = 0; i < pv.starts.size(); ++i)  // start i shows the number i + 1, opaque white
            c->markers.push_back({pv.starts[i], ascii32(std::to_string(i + 1)), Rgba8{255, 255, 255, 255}});
    }
    d.set_text("IDC_SCENARIO_DESCRIPTION_TEXT_BOX", std::u32string());
    d.show("IDC_SCENARIO_DESCRIPTION_TEXT_BOX", false);
}

// UI.md 9.7: the picks are stored as they are, then in slot order slot 0 keeps its colour and each later slot whose colour
// an earlier slot uses takes the lowest colour index no earlier slot uses; the combos show the result.
void FrontEnd::resolve_skirmish_colours(GuiDialog& d) {
    constexpr int32_t kUsed = 2;  // [port] local player + one AI
    const auto palette = static_cast<int32_t>(colour_palette_.size());
    for (int32_t p = 1; p < kUsed; ++p) {
        const auto taken = [&](int32_t col) {
            for (int32_t q = 0; q < p; ++q)
                if (slot_colour_[q] == col) return true;
            return false;
        };
        if (!taken(slot_colour_[p])) continue;
        for (int32_t col = 0; col < palette; ++col)
            if (!taken(col)) {
                slot_colour_[p] = col;
                break;
            }
    }
    char id[32];
    for (int32_t p = 0; p < kUsed; ++p) {
        std::snprintf(id, sizeof id, "IDC_PLAYER_COLOR_%d", p);
        const GuiControl* c = d.get(id);
        if (c && c->selected != slot_colour_[p]) d.set_selected(id, slot_colour_[p]);
    }
}

void FrontEnd::fill_skirmish(GuiDialog& d) {
    maps_ = p_.skirmish_maps ? p_.skirmish_maps() : std::vector<SkirmishMapInfo>{};
    colour_palette_ = p_.player_colors ? p_.player_colors() : std::vector<Rgba8>{};
    for (int32_t i = 0; i < 8; ++i) slot_colour_[i] = i;  // the default colour is the slot index
    custom_tab_ = false;  // the Official tab is active when the dialog opens
    fill_skirmish_game_types(d);
    fill_skirmish_maps(d);
    d.show("IDC_SELECT_OFFICIAL_MAPS", true);
    d.show("IDC_SELECT_CUSTOM_MAPS", true);
    std::vector<std::u32string> factions;
    for (const SkirmishFaction& f : p_.skirmish_factions)
        factions.push_back(!f.text_id.empty() && gui_->text_db() && gui_->text_db()->find(f.text_id) ? gui_->caption_text(f.text_id)
                                                                                                     : faction_label(f.name));
    char id[32];
    for (int32_t p = 0; p < 8; ++p) {
        const bool used = p < 2;  // [port] local player + one AI
        for (const char* row : {"IDC_PLAYER_NAME_%d", "IDC_PLAYER_FACTION_%d", "IDC_PLAYER_COLOR_%d", "IDC_PLAYER_TEAM_%d"}) {
            std::snprintf(id, sizeof id, row, p);
            d.show(id, used);
        }
        if (!used) continue;
        std::snprintf(id, sizeof id, "IDC_PLAYER_NAME_%d", p);
        d.set_items(id, {gui_->port_text(p == 0 ? U"Player" : U"AI")}, 0);
        std::snprintf(id, sizeof id, "IDC_PLAYER_FACTION_%d", p);
        d.set_items(id, factions, factions.empty() ? -1 : std::min<int32_t>(p, static_cast<int32_t>(factions.size()) - 1));
        std::snprintf(id, sizeof id, "IDC_PLAYER_TEAM_%d", p);
        d.set_items(id, {U"1", U"2"}, p);
    }
    fill_skirmish_colours(d);
}

// modc t08: the mod picker. The rows come from the host when the dialog opens; the selection starts on the row selected
// last, else the active row, else row 0 (the base game). Reports arrive through set_mod_report and are kept per row id.
void FrontEnd::fill_mods(GuiDialog& d) {
    mods_ = p_.mods ? p_.mods() : std::vector<ModPickerEntry>{};
    d.set_text("IDC_STATIC_MEDIUM", gui_->port_text(U"Mods"));
    d.set_text("IDC_PORT_MODS_LIST_TEXT", gui_->port_text(U"Installed"));
    d.set_text("IDC_PORT_MODS_REPORT_TEXT", gui_->port_text(U"Load report"));
    d.set_text("IDC_PORT_MODS_LAUNCH", gui_->port_text(U"Launch"));
    std::vector<std::u32string> rows;
    int32_t sel = -1, active = -1;
    for (size_t i = 0; i < mods_.size(); ++i) {
        rows.push_back(mods_[i].active ? mods_[i].label + U"  (running)" : mods_[i].label);
        if (sel < 0 && mod_chosen_ && mods_[i].id == mod_selected_) sel = static_cast<int32_t>(i);
        if (active < 0 && mods_[i].active) active = static_cast<int32_t>(i);
    }
    if (sel < 0) sel = active >= 0 ? active : (mods_.empty() ? -1 : 0);
    mod_selected_ = sel >= 0 ? mods_[static_cast<size_t>(sel)].id : std::string();
    mod_chosen_ = sel >= 0;
    d.set_items("IDC_PORT_MODS_LIST", std::move(rows), sel);
    show_mod_report(d);
}

void FrontEnd::show_mod_report(GuiDialog& d) {
    const std::vector<std::u32string>* lines = nullptr;
    for (const auto& r : mod_reports_)
        if (r.first == mod_selected_) lines = &r.second;
    bool listed = false, active = false;
    for (const ModPickerEntry& m : mods_)
        if (m.id == mod_selected_) listed = true, active = m.active;
    std::vector<std::u32string> items = lines ? *lines : std::vector<std::u32string>{gui_->port_text(U"Checking...")};
    d.set_items("IDC_PORT_MODS_REPORT", std::move(items), -1);
    d.enable("IDC_PORT_MODS_LAUNCH", lines != nullptr && listed && !active);
}

void FrontEnd::set_mod_report(std::string_view id, std::vector<std::u32string> lines) {
    bool stored = false;
    for (auto& r : mod_reports_)
        if (r.first == id) {
            r.second = std::move(lines);
            stored = true;
            break;
        }
    if (!stored) mod_reports_.emplace_back(std::string(id), std::move(lines));
    if (gui_ && id == mod_selected_)
        if (GuiDialog* d = gui_->find(kPortModsDialog)) show_mod_report(*d);
}

void FrontEnd::fill_saves(GuiDialog& d) {
    saves_ = p_.saves ? p_.saves() : std::vector<SaveGameInfo>{};
    std::vector<std::u32string> items;
    for (const SaveGameInfo& s : saves_) items.push_back(s.label.empty() ? ascii32(s.name) : s.label);
    d.set_items("IDC_SAVE_GAME_LIST", std::move(items), -1);
    d.show("IDC_CHECK_RECORDINGS", false);  // replays are not saved games [port]
}

void FrontEnd::refresh_saves() {
    if (!gui_) return;
    for (const char* n : {kSave, kLoad})
        if (GuiDialog* d = gui_->find(n)) fill_saves(*d);
}

void FrontEnd::fill_audio(GuiDialog& d, const AudioVolumes& v) {
    d.set_value("IDC_SLIDER_MASTER_VOLUME", to100(v.master));
    d.set_value("IDC_SLIDER_SOUND_EFFECTS_VOLUME", to100(v.sfx));
    d.set_value("IDC_SLIDER_DIALOG_VOLUME", to100(v.speech));
    d.set_value("IDC_SLIDER_MUSIC_VOLUME", to100(v.music));
    d.set_checked("IDC_USE_MOVIE_SUBTITLES", v.subtitles);
    d.set_items("IDC_AUDIO_3D_TECHNOLOGY_COMBOBOX", {U"miniaudio"}, 0);  // [port] one backend
}

AudioVolumes FrontEnd::read_audio(const GuiDialog& d) const {
    AudioVolumes v = audio_before_;
    if (const GuiControl* c = d.get("IDC_SLIDER_MASTER_VOLUME")) v.master = to01(c->value);
    if (const GuiControl* c = d.get("IDC_SLIDER_SOUND_EFFECTS_VOLUME")) v.sfx = to01(c->value);
    if (const GuiControl* c = d.get("IDC_SLIDER_DIALOG_VOLUME")) v.speech = to01(c->value);
    if (const GuiControl* c = d.get("IDC_SLIDER_MUSIC_VOLUME")) v.music = to01(c->value);
    if (const GuiControl* c = d.get("IDC_USE_MOVIE_SUBTITLES")) v.subtitles = c->checked;
    return v;
}

void FrontEnd::fill_video(GuiDialog& d) {
    const VideoSettings v = p_.video ? p_.video() : VideoSettings{};
    d.set_items("IDC_RESOLUTION_LIST", v.resolutions, v.resolution);
    // [port] the list holds every mode at every refresh rate: open it on the current one, kept in range so a mode near
    // the end (the desktop's own, the largest) still fills the list with the sizes below it (QA U10).
    if (const GuiControl* l = d.get("IDC_RESOLUTION_LIST"); l && l->selected > 0) gui_->scroll_list_to(d, "IDC_RESOLUTION_LIST", l->selected - 3);
    d.set_checked("IDC_WINDOWED_MODE", v.windowed);
    d.set_checked("IDC_VSYNC_ENABLED", v.vsync);
    d.set_checked("IDC_WIDESCREEN", v.widescreen);
    d.set_checked("IDC_HARDWARE_MOUSE", v.hardware_mouse);
    // [port: Gabriel] Anti-Aliasing and Detail Level are lists of named levels in the sliders' rows; Gamma stays a slider.
    hide_slider(d, "IDC_ANTI_ALIAS_SLIDER");
    hide_slider(d, "IDC_DETAIL_LEVEL_SLIDER");
    d.set_items("IDC_PORT_ANTI_ALIASING", level_items(*gui_, kAntiAliasChoices), static_cast<int32_t>(level_index(kAntiAliasChoices, v.anti_alias)));
    std::vector<std::u32string> quality;
    for (const char32_t* q : kQualityNames) quality.push_back(gui_->port_text(q));
    d.set_items("IDC_PORT_DETAIL_LEVEL", std::move(quality), static_cast<int32_t>(quality_level(current_advanced(p_, v))));
    d.set_value("IDC_GAMMA_SLIDER", v.gamma);
    d.set_text("IDC_PORT_GAMMA_VALUE", value_text(v.gamma));
    d.set_text("IDC_DEVICE_DESC_STATIC", v.device);
    vsync_on_row_ = static_cast<int32_t>(v.vsync_mode) + 1;
    d.set_items("IDC_PORT_VSYNC_MODE",
                {gui_->port_text(U"Off (no v-sync)"), gui_->port_text(U"Standard (FIFO)"),
                 gui_->port_text(v.mailbox_available ? U"Low latency (Mailbox)" : U"Mailbox (not on this driver)")},
                v.vsync ? vsync_on_row_ : 0);
    std::vector<std::u32string> caps;
    int32_t cap_sel = 0;
    for (size_t i = 0; i < std::size(kFrameCapChoices); ++i) {
        const int32_t c = kFrameCapChoices[i];
        caps.push_back(gui_->port_text(c == 0 ? U"Off" : c == kFrameCapDisplay ? U"Display refresh rate" : ascii32(std::to_string(c) + " fps")));
        if (c == v.frame_cap) cap_sel = static_cast<int32_t>(i);
    }
    d.set_items("IDC_PORT_FRAME_CAP", std::move(caps), cap_sel);
    use_ui_font(d, {"IDC_PORT_VSYNC_MODE", "IDC_PORT_FRAME_CAP", "IDC_PORT_ANTI_ALIASING", "IDC_PORT_DETAIL_LEVEL"});
    d.set_text("IDC_PORT_DISPLAY_INFO", gui_->port_text(v.display_info));
    // t47 [port]: the value column (143..175) sits inside the shipped caption box (41..187):
    // the caption ends where its value starts and is narrowed to fit, or a long one runs under the number.
    for (const char* id : {"IDC_PORT_GAMMA_VALUE"}) {
        const GuiControl* value = d.get(id);
        if (!value || !value->def) continue;
        for (size_t i = 0; i < d.size(); ++i) {
            GuiControl& c = d[i];
            if (c.type != GuiGadgetType::Text || !c.def || &c == value || c.def->w <= 0 || std::abs(c.def->y - value->def->y) > 1) continue;
            if (c.def->x < value->def->x && c.def->x + c.def->w > value->def->x)
                c.scale.x = static_cast<float>(value->def->x - c.def->x - 2) / static_cast<float>(c.def->w);
        }
    }
}

void FrontEnd::refresh_video_dialogs() {
    if (GuiDialog* vd = gui_->find(kVideo)) {
        const VideoSettings v = p_.video ? p_.video() : VideoSettings{};
        vd->set_selected("IDC_PORT_ANTI_ALIASING", static_cast<int32_t>(level_index(kAntiAliasChoices, v.anti_alias)));
        vd->set_selected("IDC_PORT_DETAIL_LEVEL", static_cast<int32_t>(quality_level(current_advanced(p_, v))));
    }
    if (GuiDialog* ad = gui_->find(kAdvancedVideo)) fill_advanced_video(*ad, current_advanced(p_, p_.video ? p_.video() : VideoSettings{}));
}

void FrontEnd::fill_advanced_video(GuiDialog& d, const AdvancedVideoSettings& s) {
    for (const AdvLevel& l : kAdvLevels) {
        hide_slider(d, l.slider);
        d.set_items(l.combo, level_items(*gui_, l.choices), static_cast<int32_t>(level_index(l.choices, s.*l.field)));
        use_ui_font(d, {l.combo});
    }
    for (const AdvCheck& c : kAdvChecks) d.set_checked(c.id, s.*c.field);
    std::vector<std::u32string> af;
    int32_t af_sel = 0;
    for (size_t i = 0; i < std::size(kAnisotropyChoices); ++i) {
        const int32_t a = kAnisotropyChoices[i];
        af.push_back(a == 0 ? gui_->port_text(U"Off") : ascii32(std::to_string(a) + "x"));
        if (a == s.anisotropy) af_sel = static_cast<int32_t>(i);
    }
    d.set_items("IDC_PORT_ANISOTROPY", std::move(af), af_sel);
    d.set_checked("IDC_PORT_AMBIENT_OCCLUSION", s.ambient_occlusion);
    use_ui_font(d, {"IDC_PORT_ANISOTROPY"});
    // QA D8 [port]: the check rows are widened up to the combo column, so "Heat Distortion" is not narrowed to fit 152 units.
    for (const AdvCheck& k : kAdvChecks)
        if (GuiControl* c = d.get(k.id); c && c->def->w > 0) c->scale.x = kAdvCheckWidth / static_cast<float>(c->def->w);
}

AdvancedVideoSettings FrontEnd::read_advanced_video(const GuiDialog& d) const {
    AdvancedVideoSettings s = p_.advanced_video ? p_.advanced_video() : AdvancedVideoSettings{};
    // A list still on the level the stored value falls in keeps that exact value (an older profile's 55 is not rewritten to 75).
    for (const AdvLevel& l : kAdvLevels)
        if (const std::optional<size_t> row = picked(d, l.combo, l.choices.size()); row && *row != level_index(l.choices, s.*l.field))
            s.*l.field = l.choices[*row].value;
    for (const AdvCheck& k : kAdvChecks)
        if (const GuiControl* c = d.get(k.id)) s.*k.field = c->checked;
    if (const GuiControl* c = d.get("IDC_PORT_ANISOTROPY"); c && c->selected >= 0 && static_cast<size_t>(c->selected) < std::size(kAnisotropyChoices))
        s.anisotropy = kAnisotropyChoices[static_cast<size_t>(c->selected)];
    if (const GuiControl* c = d.get("IDC_PORT_AMBIENT_OCCLUSION")) s.ambient_occlusion = c->checked;
    return s;
}

float scroll_speed_option_from_slider(int32_t slider) {
    const int32_t v = std::clamp(slider, 0, 100);
    constexpr int32_t d = kDefaultScrollSpeedSlider;
    if (v <= d) return static_cast<float>(v) / static_cast<float>(d);
    return 1.0f + 2.0f * static_cast<float>(v - d) / static_cast<float>(100 - d);
}

void FrontEnd::fill_gameplay(GuiDialog& d, const GameplaySettings& g) {
    d.set_value("IDC_GAME_SPEED_SLIDER", g.game_speed);
    d.set_value("IDC_STRATEGIC_GAME_SPEED_SLIDER", g.strategic_speed);
    d.set_value("IDC_SCROLL_SPEED_SLIDER", g.scroll_speed);
    d.set_checked("IDC_AUTORESOLVE", g.ask_autoresolve);
    d.set_checked("IDC_DELAY_TOOLTIPS", g.delay_tooltips);
    d.set_checked("IDC_ABSOLUTE_SCROLLING", g.absolute_scroll);
    d.set_checked("IDC_ALTERNATE_MOUSE_CONTROLS", g.alternate_mouse);
    d.set_checked("IDC_RIGHT_BUTTON_SCROLL", g.right_button_scroll);
    d.set_checked("IDC_BOTH_MOUSE_BUTTONS_ROTATE", g.both_buttons_rotate);
    d.set_checked("IDC_SIMPLIFIED_FACTION_COLORS", g.simplified_colors);
    d.set_checked("IDC_ABILITIES_DEFAULT_TO_AUTOFIRE", g.abilities_autofire);
    const char* radios[3] = {"IDC_RADIO_EASY", "IDC_RADIO_MEDIUM", "IDC_RADIO_HARD"};
    d.set_checked(radios[std::clamp(g.difficulty, 0, 2)], true);
    for (int32_t i = 0; i < 3; ++i) d.set_text(radios[i], text(kDifficultyKeys[i]));
}

GameplaySettings FrontEnd::read_gameplay(const GuiDialog& d) const {
    GameplaySettings g;
    auto val = [&](const char* id, int32_t& out) {
        if (const GuiControl* c = d.get(id)) out = c->value;
    };
    auto chk = [&](const char* id, bool& out) {
        if (const GuiControl* c = d.get(id)) out = c->checked;
    };
    val("IDC_GAME_SPEED_SLIDER", g.game_speed);
    val("IDC_STRATEGIC_GAME_SPEED_SLIDER", g.strategic_speed);
    val("IDC_SCROLL_SPEED_SLIDER", g.scroll_speed);
    chk("IDC_AUTORESOLVE", g.ask_autoresolve);
    chk("IDC_DELAY_TOOLTIPS", g.delay_tooltips);
    chk("IDC_ABSOLUTE_SCROLLING", g.absolute_scroll);
    chk("IDC_ALTERNATE_MOUSE_CONTROLS", g.alternate_mouse);
    chk("IDC_RIGHT_BUTTON_SCROLL", g.right_button_scroll);
    chk("IDC_BOTH_MOUSE_BUTTONS_ROTATE", g.both_buttons_rotate);
    chk("IDC_SIMPLIFIED_FACTION_COLORS", g.simplified_colors);
    chk("IDC_ABILITIES_DEFAULT_TO_AUTOFIRE", g.abilities_autofire);
    g.difficulty = difficulty_of(d);
    return g;
}

void FrontEnd::fill_keyboard(GuiDialog& d) {
    const int32_t page = keyboard_page(d.name());
    const char* radios[5] = {"IDC_KEYBOARD_OPTIONS_1", "IDC_KEYBOARD_OPTIONS_2", "IDC_KEYBOARD_OPTIONS_3",
                             "IDC_KEYBOARD_OPTIONS_4", "IDC_KEYBOARD_OPTIONS_5"};
    if (page >= 0) d.set_checked(radios[page], true);
    for (size_t i = 0; i < d.size(); ++i) {
        GuiControl& c = d[i];
        if (c.type != GuiGadgetType::EditBox) continue;
        const int32_t slot = keyboard_edit_slot(c.id());
        c.read_only = true;
        c.text = slot >= 0 ? hotkey_label(keys_work_.binding(static_cast<uint16_t>(slot))) : std::u32string();
        if (slot < 0) c.enabled = false;
    }
}

void FrontEnd::setup(GuiDialog& d) {
    const std::string_view n = d.name();
    if (ascii_iequals(n, kMain)) {
        d.show("IDC_MAIN_MENU_PLAY_DEMO", p_.demo);
        d.show("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES", !p_.demo);
        // modc t08: the port's Mods button (kPortDialogExtensionsRc) opens the mod picker.
        const bool mods = p_.mods && gui_->has_dialog(kPortModsDialog);
        d.show("IDC_PORT_MAIN_MENU_MODS", mods);
        if (mods) d.set_text("IDC_PORT_MAIN_MENU_MODS", gui_->port_text(U"Mods"));
    } else if (ascii_iequals(n, kSingle)) {
        d.show("IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", false);
        d.enable("IDC_NEW_CAMPAIGN_BUTTON", !menu_.story.empty());
        d.enable("IDC_CUSTOM_CAMPAIGN_BUTTON", !menu_.custom.empty());
    } else if (ascii_iequals(n, kCampaignSelect)) {
        d.set_value("IDC_CAMPAIGN_DIFFICULTY_SLIDER", 1);
        fill_campaign_select(d);
    } else if (ascii_iequals(n, kCustomStaging)) {
        d.set_checked("IDC_RADIO_CUSTOM_MEDIUM", true);
        for (int32_t i = 0; i < 3; ++i) {
            const char* labels[3] = {"IDC_DIFFICULTY1", "IDC_DIFFICULTY2", "IDC_DIFFICULTY3"};
            d.set_text(labels[i], text(kDifficultyKeys[i]));
        }
        fill_custom_staging(d);
    } else if (ascii_iequals(n, kSkirmish)) {
        fill_skirmish(d);
    } else if (ascii_iequals(n, kTutorial)) {
        std::vector<std::u32string> items;
        for (const CampaignSetInfo& s : menu_.tutorials) items.push_back(text(s.text_id.empty() ? s.set : s.text_id));
        d.set_items("IDC_STAGING_MAP_LIST", std::move(items), menu_.tutorials.empty() ? -1 : 0);
    } else if (ascii_iequals(n, kOptions) || ascii_iequals(n, kInGameOptions)) {
        // [port: Gabriel] the hidden Mods / Network slot opens the camera and controls dialog when it is merged.
        const char* slot = ascii_iequals(n, kOptions) ? "IDC_MAIN_OPTIONS_MODS" : "IDC_IN_GAME_OPTIONS_NETWORK";
        const bool port = gui_->has_dialog(kPortControlsDialog);
        d.show(slot, port);
        if (port) d.set_text(slot, gui_->port_text(U"Camera and Controls"));
    } else if (is_port_controls(n)) {
        fill_port_controls(d, p_.camera_settings ? p_.camera_settings() : CameraSettings{});
    } else if (ascii_iequals(n, kPortModsDialog)) {
        fill_mods(d);
    } else if (ascii_iequals(n, kAudio)) {
        audio_before_ = p_.audio ? p_.audio() : AudioVolumes{};
        fill_audio(d, audio_before_);
    } else if (ascii_iequals(n, kVideo)) {
        fill_video(d);
    } else if (ascii_iequals(n, kAdvancedVideo)) {
        AdvancedVideoSettings s = p_.advanced_video ? p_.advanced_video() : AdvancedVideoSettings{};
        // t43: opened from the Video dialog, a quality level picked there but not yet applied shows here as the preset it will be.
        if (const GuiDialog* vd = gui_->find(kVideo))
            if (const std::optional<size_t> level = picked(*vd, "IDC_PORT_DETAIL_LEVEL", kQualityCount); level && *level != quality_level(s))
                set_advanced_detail(s, kQualityPresets[*level]);
        fill_advanced_video(d, s);
    } else if (ascii_iequals(n, kGameplay)) {
        fill_gameplay(d, p_.gameplay ? p_.gameplay() : GameplaySettings{});
    } else if (keyboard_page(n) >= 0) {
        fill_keyboard(d);
    } else if (ascii_iequals(n, kSave) || ascii_iequals(n, kLoad)) {
        fill_saves(d);
        if (ascii_iequals(n, kSave)) {  // N1 / t09: a game that cannot be saved now shows why instead of a name
            const std::u32string why = p_.save_blocker ? p_.save_blocker() : std::u32string();
            save_blocked_ = !why.empty();
            d.enable("IDC_SAVE_GAME_BUTTON", !save_blocked_);
            d.enable("IDC_SAVE_GAME_NAME_EDIT", !save_blocked_);
            d.set_text("IDC_SAVE_GAME_NAME_EDIT", why);
        }
    } else if (ascii_iequals(n, kQuit)) {
        const bool overwrite = quit_action_ == "overwrite", del = quit_action_ == "delete";
        d.set_text("IDC_CAPTION", overwrite ? text_or("TEXT_SAVE_CONFIRM", U"Overwrite Save Game?")
                                  : del     ? text_or("TEXT_DELETE_CONFIRM", U"Delete saved game?")
                                            : text("TEXT_BUTTON_QUIT_GAME"));
        d.set_text("IDC_DIALOG_BOX_BUTTON_LEFT", text("TEXT_BUTTON_YES"));
        d.set_text("IDC_DIALOG_BOX_BUTTON_RIGHT", text("TEXT_BUTTON_NO"));
        for (const char* u : {"IDC_USER_TEXT_STRING_1", "IDC_USER_TEXT_STRING_2", "IDC_USER_TEXT_STRING_3"}) d.show(u, false);
        if (overwrite || del) {  // N1 / t09: the save the question is about
            d.set_text("IDC_USER_TEXT_STRING_1", ascii32(confirm_save_));
            d.show("IDC_USER_TEXT_STRING_1", true);
        }
    } else if (ascii_iequals(n, kPause)) {
        d.enable("IDC_SAVE_GAME", in_game_);
        if (const float gap = pause_menu_gap(d); gap > 0.0f) {
            for (const char* id : {"IDC_GAME_OPTIONS_QUIT", "IDC_GAME_OPTIONS_RESUME"})
                if (GuiControl* c = d.get(id)) c->offset.y = gap;
            gui_->trim_dialog_height(d, gap);
        }
    }
}

// ---- t12: resolution-change confirmation ------------------------------------------------------------------------
void FrontEnd::show_confirm_countdown(GuiDialog& d) {
    const int32_t secs = static_cast<int32_t>(std::ceil(std::max(0.0, res_deadline_ - now_)));
    if (secs == res_shown_) return;
    res_shown_ = secs;
    std::u32string t = text_or("TEXT_REVERTING_IN_X_SECONDS", U"Reverting in %d seconds.");
    const std::u32string num = ascii32(std::to_string(secs));
    if (const size_t at = t.find(U"%d"); at != std::u32string::npos) t.replace(at, 2, num);
    d.set_text("IDC_USER_TEXT_STRING_2", t);
}

void FrontEnd::begin_resolution_confirm(const VideoSettings& applied, int32_t previous_resolution, std::vector<MenuAction>& out) {
    res_keep_ = applied;
    res_keep_.unconfirmed = false;
    res_revert_ = res_keep_;
    res_revert_.resolution = previous_resolution;
    MenuAction a = act(MenuAction::Kind::ApplyVideo);  // applied now, stored only when kept
    a.video = applied;
    a.video.unconfirmed = true;
    out.push_back(std::move(a));
    GuiDialog* d = push(kResConfirm);
    if (!d) {  // no confirmation box in the script: keep (nothing to ask with)
        end_resolution_confirm(true, out);
        return;
    }
    res_confirm_ = true;
    res_deadline_ = now_ + kResolutionConfirmSeconds;
    res_shown_ = -1;
    d->set_text("IDC_CAPTION", text_or("TEXT_VIDEO_OPTIONS", U"Video"));
    d->set_text("IDC_USER_TEXT_STRING_1",
                text_or("TEXT_RESOLUTION_CHANGED_CONFIRMATION", U"Your screen resolution has been changed.  Do you want to keep these settings?"));
    d->set_text("IDC_DIALOG_BOX_BUTTON_LEFT", text_or("TEXT_BUTTON_YES", U"Yes"));
    d->set_text("IDC_DIALOG_BOX_BUTTON_RIGHT", text_or("TEXT_BUTTON_NO", U"No"));
    d->show("IDC_USER_TEXT_STRING_3", false);
    show_confirm_countdown(*d);
}

void FrontEnd::end_resolution_confirm(bool keep, std::vector<MenuAction>& out) {
    const bool was_open = res_confirm_;
    res_confirm_ = false;
    if (was_open && top() == kResConfirm) pop();
    MenuAction a = act(MenuAction::Kind::ApplyVideo);
    a.video = keep ? res_keep_ : res_revert_;
    out.push_back(std::move(a));
    if (!keep)  // the video dialog underneath (Apply, not OK) shows the restored selection again
        if (GuiDialog* v = gui_->find(kVideo)) {
            v->set_selected("IDC_RESOLUTION_LIST", res_revert_.resolution);
            if (GuiControl* l = v->get("IDC_RESOLUTION_LIST"); l && l->selected > 0) l->top = std::max(0, l->selected - 3);
        }
}

void FrontEnd::on_cancel(const GuiEvent& e, std::vector<MenuAction>& out) {
    const std::string_view n = e.dialog;
    if (res_confirm_ && ascii_iequals(n, kResConfirm)) {  // Esc = No
        end_resolution_confirm(false, out);
        return;
    }
    if (ascii_iequals(n, kResConfirm)) return;  // a late Esc after the dialog already answered
    if (ascii_iequals(n, kMain)) return;  // Esc on the main menu does nothing [port]
    if (ascii_iequals(n, kPause)) {
        close_pause_menu();
        out.push_back(act(MenuAction::Kind::ResumeGame));
        return;
    }
    if (ascii_iequals(n, kSingle) || ascii_iequals(n, kOptions) || ascii_iequals(n, kPortModsDialog)) {
        go(kMain);
        return;
    }
    if (ascii_iequals(n, kCampaignSelect) || ascii_iequals(n, kCustomStaging) || ascii_iequals(n, kSkirmish) || ascii_iequals(n, kTutorial)) {
        go(kSingle);
        return;
    }
    if (ascii_iequals(n, kAudio)) {
        out.push_back(act(MenuAction::Kind::PreviewAudio));
        out.back().audio = audio_before_;
    }
    pop();  // sub-dialogs return to what opened them
}

void FrontEnd::on_change(const GuiEvent& e, std::vector<MenuAction>& out) {
    GuiDialog* d = gui_->find(e.dialog);
    if (!d) return;
    const std::string_view n = e.dialog;
    if (ascii_iequals(n, kPortModsDialog)) {  // modc t08: a picker row selected -> its report (cached, else the host checks it)
        if (e.control == "IDC_PORT_MODS_LIST" && e.kind == GuiEvent::Kind::SelectionChanged && e.value >= 0 &&
            static_cast<size_t>(e.value) < mods_.size() && mods_[static_cast<size_t>(e.value)].id != mod_selected_) {
            mod_selected_ = mods_[static_cast<size_t>(e.value)].id;
            show_mod_report(*d);
            MenuAction a = act(MenuAction::Kind::CheckMod);
            a.mod = mod_selected_;
            out.push_back(std::move(a));
        }
        return;
    }
    if (ascii_iequals(n, kCampaignSelect)) fill_campaign_select(*d);
    else if (ascii_iequals(n, kCustomStaging) && e.control == "IDC_STAGING_MAP_LIST") fill_custom_staging(*d);
    else if (ascii_iequals(n, kSkirmish) && e.kind == GuiEvent::Kind::SelectionChanged) {
        if (e.control == "IDC_GAME_TYPE_LIST" && e.value >= 0 && e.value < 2) {
            if (space_type_ != (e.value == 1)) {
                space_type_ = e.value == 1;
                fill_skirmish_maps(*d);
            }
        } else if (e.control == "IDC_STAGING_MAP_LIST") {
            // UI.md 9.7: every selection change stores the row's map for the current game type.
            if (e.value >= 0 && static_cast<size_t>(e.value) < map_view_.size())
                last_map_[space_type_ ? 1 : 0] = maps_[map_view_[static_cast<size_t>(e.value)]].map;
            fill_skirmish_preview(*d);
        } else if (ascii_istarts_with(e.control, "IDC_PLAYER_COLOR_") && e.control.size() == 18 && e.value >= 0 &&
                   static_cast<size_t>(e.value) < colour_palette_.size()) {
            // UI.md 9.7: the pick is stored as it is, then the clash rule runs in slot order.
            const int32_t slot = e.control.back() - '0';
            if (slot >= 0 && slot < 2) {
                slot_colour_[slot] = e.value;
                resolve_skirmish_colours(*d);
            }
        }
    }
    else if (ascii_iequals(n, kAudio) && e.kind == GuiEvent::Kind::ValueChanged) {
        MenuAction a = act(MenuAction::Kind::PreviewAudio);
        a.audio = read_audio(*d);
        out.push_back(std::move(a));
    } else if (ascii_iequals(n, kSave) && e.control == "IDC_SAVE_GAME_LIST" && e.value >= 0 &&
               static_cast<size_t>(e.value) < saves_.size()) {
        // N1 / t09: the save's name (the file), not its label.
        if (!save_blocked_) d->set_text("IDC_SAVE_GAME_NAME_EDIT", ascii32(saves_[static_cast<size_t>(e.value)].name));
    } else if (ascii_iequals(n, kLoad) && e.kind == GuiEvent::Kind::DoubleClicked && e.value >= 0 &&
               static_cast<size_t>(e.value) < saves_.size()) {
        MenuAction a = act(MenuAction::Kind::LoadGame);
        a.save = saves_[static_cast<size_t>(e.value)].name;
        out.push_back(std::move(a));
    } else if (is_port_controls(n) && e.kind == GuiEvent::Kind::ValueChanged) {  // [port: Gabriel] FOV value labels
        if (e.control == "IDC_PORT_GALACTIC_FOV_SLIDER") d->set_text("IDC_PORT_GALACTIC_FOV_VALUE", fov_text(static_cast<float>(e.value)));
        if (e.control == "IDC_PORT_TACTICAL_FOV_SLIDER") d->set_text("IDC_PORT_TACTICAL_FOV_VALUE", fov_text(static_cast<float>(e.value)));
    } else if (ascii_iequals(n, kVideo) && e.kind == GuiEvent::Kind::ValueChanged) {  // [port] the value beside the Gamma slider (U8)
        if (e.control == "IDC_GAMMA_SLIDER") d->set_text("IDC_PORT_GAMMA_VALUE", value_text(e.value));
    } else if (ascii_iequals(n, kVideo) && e.kind == GuiEvent::Kind::Checked && e.control == "IDC_VSYNC_ENABLED") {  // U9: one setting
        d->set_selected("IDC_PORT_VSYNC_MODE", e.value != 0 ? vsync_on_row_ : 0);
    } else if (ascii_iequals(n, kVideo) && e.kind == GuiEvent::Kind::SelectionChanged && e.control == "IDC_PORT_VSYNC_MODE") {
        if (e.value > 0) vsync_on_row_ = e.value;
        d->set_checked("IDC_VSYNC_ENABLED", e.value > 0);
    } else if (keyboard_page(n) >= 0 && e.kind == GuiEvent::Kind::Checked && ascii_istarts_with(e.control, "IDC_KEYBOARD_OPTIONS_")) {
        const int32_t page = e.control.back() - '1';
        if (page >= 0 && page < 5 && !ascii_iequals(n, kKeyboardPages[page])) go(kKeyboardPages[page]);
    }
}

void FrontEnd::on_click(const GuiEvent& e, std::vector<MenuAction>& out) {
    const std::string_view n = e.dialog, c = e.control;
    GuiDialog* d = gui_->find(n);
    auto is = [&](std::string_view id) { return ascii_iequals(c, id); };
    if (ascii_iequals(n, kMain)) {
        if (is("IDC_MAIN_MENU_TUTORIAL")) go(kTutorial);
        else if (is("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES") || is("IDC_MAIN_MENU_PLAY_DEMO")) go(kSingle);
        else if (is("IDC_MAIN_MENU_MULTIPLAYER")) out.push_back(act(MenuAction::Kind::OpenMultiplayer));
        else if (is("IDC_LOAD_GAME_MAIN")) push(kLoad);
        else if (is("IDC_OPTIONS")) go(kOptions);
        else if (is("IDC_PORT_MAIN_MENU_MODS")) {  // modc t08
            go(kPortModsDialog);
            if (gui_->find(kPortModsDialog)) {
                MenuAction a = act(MenuAction::Kind::CheckMod);
                a.mod = mod_selected_;
                out.push_back(std::move(a));
            }
        } else if (is("IDC_MENU_CREDITS")) out.push_back(act(MenuAction::Kind::ShowCredits));
        else if (is("IDC_MAIN_MENU_EXIT2")) {
            quit_action_ = "exit";
            push(kQuit);
        }
    } else if (ascii_iequals(n, kSingle)) {
        if (is("IDC_NEW_CAMPAIGN_BUTTON")) go(kCampaignSelect);
        else if (is("IDC_CUSTOM_CAMPAIGN_BUTTON")) go(kCustomStaging);
        else if (is("IDC_SKIRMISH_GAMES_BUTTON")) go(kSkirmish);
        else if (is("IDC_BACK_BUTTON_MAIN")) go(kMain);
        else if (is("IDC_MENU_CREDITS")) out.push_back(act(MenuAction::Kind::ShowCredits));
    } else if (ascii_iequals(n, kCampaignSelect) && d) {
        if (is("IDC_CANCEL_BUTTON")) go(kSingle);
        else if (is("IDC_PLAY_FACTION_A_BUTTON") || is("IDC_PLAY_FACTION_B_BUTTON")) {
            const GuiControl* list = d->get("IDC_CAMPAIGNS_LIST");
            const size_t fi = is("IDC_PLAY_FACTION_A_BUTTON") ? 0 : 1;
            if (list && list->selected >= 0 && static_cast<size_t>(list->selected) < menu_.story.size()) {
                const CampaignSetInfo& s = menu_.story[static_cast<size_t>(list->selected)];
                if (fi < s.factions.size()) {
                    MenuAction a = act(MenuAction::Kind::StartCampaign);
                    a.campaign = s.factions[fi].campaign;
                    a.faction = s.factions[fi].faction;
                    a.difficulty = difficulty_of(*d);
                    out.push_back(std::move(a));
                }
            }
        }
    } else if (ascii_iequals(n, kCustomStaging) && d) {
        if (is("IDC_CANCEL_BUTTON")) go(kSingle);
        else if (is("IDC_SELECT_OPTIONS")) push(kAdvanced);
        else if (is("IDC_PLAY_BUTTON")) {
            const GuiControl* list = d->get("IDC_STAGING_MAP_LIST");
            const GuiControl* fc = d->get("IDC_FACTION_COMBO");
            if (list && list->selected >= 0 && static_cast<size_t>(list->selected) < menu_.custom.size()) {
                const CampaignSetInfo& s = menu_.custom[static_cast<size_t>(list->selected)];
                const size_t fi = fc && fc->selected >= 0 ? static_cast<size_t>(fc->selected) : 0;
                if (fi < s.factions.size()) {
                    MenuAction a = act(MenuAction::Kind::StartCampaign);
                    a.campaign = s.factions[fi].campaign;
                    a.faction = s.factions[fi].faction;
                    a.difficulty = difficulty_of(*d);
                    out.push_back(std::move(a));
                }
            }
        }
    } else if (ascii_iequals(n, kAdvanced)) {
        if (is("IDC_CANCEL_BUTTON") || is("IDC_BUTTON_ACCEPT")) pop();
    } else if (ascii_iequals(n, kSkirmish) && d) {
        if (is("IDC_CANCEL_BUTTON")) go(kSingle);
        else if (is("IDC_SELECT_OPTIONS")) push(kAdvanced);
        else if (is("IDC_SELECT_OFFICIAL_MAPS") || is("IDC_SELECT_CUSTOM_MAPS")) {
            custom_tab_ = is("IDC_SELECT_CUSTOM_MAPS");
            fill_skirmish_maps(*d);
        } else if (is("IDC_SKIRMISH_START_GAME_BUTTON")) {
            const GuiControl* list = d->get("IDC_STAGING_MAP_LIST");
            if (list && list->selected >= 0 && static_cast<size_t>(list->selected) < map_view_.size()) {
                MenuAction a = act(MenuAction::Kind::StartSkirmish);
                a.map = maps_[map_view_[static_cast<size_t>(list->selected)]].map;
                for (int32_t p = 0; p < 2; ++p) {
                    a.colors.push_back(slot_colour_[p]);
                    char id[32];
                    std::snprintf(id, sizeof id, "IDC_PLAYER_FACTION_%d", p);
                    const GuiControl* f = d->get(id);
                    const size_t fi = f && f->selected >= 0 ? static_cast<size_t>(f->selected) : 0;
                    a.factions.push_back(fi < p_.skirmish_factions.size() ? p_.skirmish_factions[fi].name : std::string());
                }
                out.push_back(std::move(a));
            }
        }
    } else if (ascii_iequals(n, kTutorial) && d) {
        if (is("IDC_CANCEL_BUTTON")) go(kMain);
        else if (is("IDC_PLAY_BUTTON")) {
            const GuiControl* list = d->get("IDC_STAGING_MAP_LIST");
            if (list && list->selected >= 0 && static_cast<size_t>(list->selected) < menu_.tutorials.size() &&
                !menu_.tutorials[static_cast<size_t>(list->selected)].factions.empty()) {
                const CampaignSetInfo& s = menu_.tutorials[static_cast<size_t>(list->selected)];
                MenuAction a = act(MenuAction::Kind::StartTutorial);
                a.campaign = s.factions.front().campaign;
                a.faction = s.factions.front().faction;
                out.push_back(std::move(a));
            }
        }
    } else if (ascii_iequals(n, kOptions) || ascii_iequals(n, kInGameOptions)) {
        if (is("IDC_MAIN_OPTIONS_AUDIO") || is("IDC_IN_GAME_OPTIONS_AUDIO")) push(kAudio);
        else if (is("IDC_MAIN_OPTIONS_VIDEO") || is("IDC_IN_GAME_OPTIONS_VIDEO")) push(kVideo);
        else if (is("IDC_MAIN_OPTIONS_GAMEPLAY") || is("IDC_IN_GAME_OPTIONS_GAMEPLAY")) push(kGameplay);
        else if (is("IDC_MAIN_OPTIONS_KEYBOARD") || is("IDC_IN_GAME_OPTIONS_KEYBOARD")) {
            keys_work_ = p_.hotkeys ? *p_.hotkeys : HotkeyTable{};
            keys_dirty_ = false;
            push(kKeyboardPages[0]);
        } else if (is("IDC_MAIN_OPTIONS_MODS") || is("IDC_IN_GAME_OPTIONS_NETWORK")) {  // [port: Gabriel]
            keys_work_ = p_.hotkeys ? *p_.hotkeys : HotkeyTable{};
            keys_dirty_ = false;
            push(kPortControlsDialog);
        } else if (is("IDC_OK_MAIN")) go(kMain);
        else if (is("IDC_OK_IN_GAME")) pop();
        else if (is("IDC_MENU_CREDITS")) out.push_back(act(MenuAction::Kind::ShowCredits));
    } else if (ascii_iequals(n, kAudio) && d) {
        if (is("IDC_OK")) {
            MenuAction a = act(MenuAction::Kind::ApplyAudio);
            a.audio = read_audio(*d);
            out.push_back(std::move(a));
            pop();
        } else if (is("IDC_CANCEL")) {
            MenuAction a = act(MenuAction::Kind::PreviewAudio);
            a.audio = audio_before_;
            out.push_back(std::move(a));
            pop();
        } else if (is("IDC_RESTORE_DEFAULT")) {
            fill_audio(*d, AudioVolumes{});
            MenuAction a = act(MenuAction::Kind::PreviewAudio);
            a.audio = read_audio(*d);
            out.push_back(std::move(a));
        }
    } else if (ascii_iequals(n, kVideo) && d) {
        if (is("IDC_OK") || is("IDC_APPLY")) {
            MenuAction a = act(MenuAction::Kind::ApplyVideo);
            VideoSettings v = p_.video ? p_.video() : VideoSettings{};
            const int32_t previous_resolution = v.resolution;  // the mode in use now (provider)
            if (const GuiControl* l = d->get("IDC_RESOLUTION_LIST")) v.resolution = l->selected;
            auto chk = [&](const char* id, bool& o) {
                if (const GuiControl* x = d->get(id)) o = x->checked;
            };
            auto val = [&](const char* id, int32_t& o) {
                if (const GuiControl* x = d->get(id)) o = x->value;
            };
            // [port: Gabriel] A list still on the level the stored value falls in keeps that exact value; one moved stores its
            // level's value. A new quality level (not Custom) also asks the host to set the seven advanced options to it.
            if (const std::optional<size_t> row = picked(*d, "IDC_PORT_ANTI_ALIASING", std::size(kAntiAliasChoices)); row && *row != level_index(kAntiAliasChoices, v.anti_alias))
                v.anti_alias = kAntiAliasChoices[*row].value;
            if (const std::optional<size_t> row = picked(*d, "IDC_PORT_DETAIL_LEVEL", kQualityCount); row && *row != quality_level(current_advanced(p_, v))) {
                v.detail = kQualityPresets[*row];
                v.detail_picked = true;
            }
            chk("IDC_WINDOWED_MODE", v.windowed);
            chk("IDC_VSYNC_ENABLED", v.vsync);
            // U9: row 0 is Off (the check box above says so); the mode is remembered while v-sync is off.
            if (const GuiControl* x = d->get("IDC_PORT_VSYNC_MODE"); x && x->selected > 0 && static_cast<size_t>(x->selected) <= kVsyncModeCount)
                v.vsync_mode = static_cast<VsyncMode>(x->selected - 1);
            if (const GuiControl* x = d->get("IDC_PORT_FRAME_CAP"); x && x->selected >= 0 && static_cast<size_t>(x->selected) < std::size(kFrameCapChoices))
                v.frame_cap = kFrameCapChoices[static_cast<size_t>(x->selected)];
            chk("IDC_WIDESCREEN", v.widescreen);
            chk("IDC_HARDWARE_MOUSE", v.hardware_mouse);
            val("IDC_GAMMA_SLIDER", v.gamma);
            // t12: a different resolution / refresh rate asks "keep these settings?" first (the Yes answer stores it).
            const bool confirm = v.resolution >= 0 && v.resolution != previous_resolution && !res_confirm_;
            if (confirm) {
                if (is("IDC_OK")) pop();
                begin_resolution_confirm(v, previous_resolution, out);
                return;
            }
            a.video = std::move(v);
            out.push_back(std::move(a));
            if (is("IDC_OK")) pop();
        } else if (is("IDC_CANCEL")) {
            pop();
        } else if (is("IDC_ADVANCED")) {
            push(kAdvancedVideo);
        } else if (is("IDC_AUTO_DETECT")) {
            out.push_back(act(MenuAction::Kind::AutoDetectVideo));
        }
    } else if (ascii_iequals(n, kAdvancedVideo) && d) {
        if (is("IDC_OK") || is("IDC_APPLY")) {
            MenuAction a = act(MenuAction::Kind::ApplyAdvancedVideo);
            a.advanced = read_advanced_video(*d);
            // t43: the Video dialog underneath shows the quality level the seven now make (the host stores the same, so its
            // Accept is no new preset).
            if (GuiDialog* vd = gui_->find(kVideo)) vd->set_selected("IDC_PORT_DETAIL_LEVEL", static_cast<int32_t>(quality_level(a.advanced)));
            out.push_back(std::move(a));
            if (is("IDC_OK")) pop();
        } else if (is("IDC_AUTO_DETECT")) {
            out.push_back(act(MenuAction::Kind::AutoDetectVideo));
        } else if (is("IDC_CANCEL")) {
            pop();
        }
    } else if (ascii_iequals(n, kGameplay) && d) {
        if (is("IDC_BUTTON_ACCEPT")) {
            MenuAction a = act(MenuAction::Kind::ApplyGameplay);
            a.gameplay = read_gameplay(*d);
            out.push_back(std::move(a));
            pop();
        } else if (is("IDC_CANCEL_BUTTON")) {
            pop();
        } else if (is("IDC_DEFAULTS_BUTTON")) {
            fill_gameplay(*d, GameplaySettings{});
        }
    } else if (keyboard_page(n) >= 0 && d) {
        if (is("IDC_APPLY")) {
            if (p_.hotkeys) *p_.hotkeys = keys_work_;
            if (p_.hotkeys && p_.bindings) p_.hotkeys->apply_to_bindings(*p_.bindings);
            keys_dirty_ = false;
            out.push_back(act(MenuAction::Kind::KeybindingsChanged));
            pop();
        } else if (is("IDC_OK")) {  // Back: discard unapplied edits
            pop();
        } else if (is("IDC_RESTORE_DEFAULTS")) {
            keys_work_.reset_defaults();
            keys_dirty_ = true;
            fill_keyboard(*d);
        }
    } else if (ascii_iequals(n, kPortModsDialog) && d) {  // modc t08
        const GuiControl* launch = d->get("IDC_PORT_MODS_LAUNCH");
        if (is("IDC_CANCEL_BUTTON")) {
            go(kMain);
        } else if (is("IDC_PORT_MODS_LAUNCH") && launch && launch->enabled) {
            MenuAction a = act(MenuAction::Kind::LaunchMod);
            a.mod = mod_selected_;
            out.push_back(std::move(a));
        }
    } else if (is_port_controls(n) && d) {  // [port: Gabriel]
        if (is("IDC_BUTTON_ACCEPT")) {
            MenuAction a = act(MenuAction::Kind::ApplyCamera);
            a.camera = read_port_controls(*d);
            if (p_.hotkeys) *p_.hotkeys = keys_work_;
            if (p_.hotkeys && p_.bindings) p_.hotkeys->apply_to_bindings(*p_.bindings);
            if (p_.bindings) p_.bindings->set_queue_mods(a.camera.queue_mods);
            keys_dirty_ = false;
            out.push_back(std::move(a));
            out.push_back(act(MenuAction::Kind::KeybindingsChanged));
            pop();
        } else if (is("IDC_CANCEL_BUTTON")) {
            pop();
        } else if (is("IDC_DEFAULTS_BUTTON")) {
            const HotkeyTable defaults;
            for (const char* s : {"Pause", "CameraYawLeft", "CameraYawRight", "CameraPitchUp", "CameraPitchDown", "CameraRollLeft",
                                  "CameraRollRight", "ScrollUp", "ScrollDown", "ScrollLeft", "ScrollRight"}) {
                const auto slot = static_cast<uint16_t>(hotkey_slot(s));
                keys_work_.bind(slot, defaults.binding(slot));
            }
            keys_dirty_ = true;
            fill_port_controls(*d, CameraSettings{});
        }
    } else if (ascii_iequals(n, kPause)) {
        if (is("IDC_SAVE_GAME")) push(kSave);
        else if (is("IDC_LOAD_GAME")) push(kLoad);
        else if (is("IDC_INGAME_OPTIONS")) push(kInGameOptions);
        else if (is("IDC_GAME_OPTIONS_QUIT")) {
            quit_action_ = "main";
            push(kQuit);
        } else if (is("IDC_GAME_OPTIONS_RESUME")) {
            close_pause_menu();
            out.push_back(act(MenuAction::Kind::ResumeGame));
        }
    } else if (ascii_iequals(n, kSave) && d) {
        if (is("IDC_SAVE_CANCEL_BUTTON")) pop();
        else if (is("IDC_SAVE_GAME_BUTTON") && !save_blocked_) {
            const GuiControl* ed = d->get("IDC_SAVE_GAME_NAME_EDIT");
            std::string name;
            if (ed)
                for (char32_t ch : ed->text) name.push_back(ch < 0x80 ? static_cast<char>(ch) : '_');
            if (!name.empty() && p_.save_exists && p_.save_exists(name)) {  // N1 / t09: overwrite? (Yes = SaveGame)
                confirm_save_ = name;
                quit_action_ = "overwrite";
                push(kQuit);
            } else if (!name.empty()) {
                MenuAction a = act(MenuAction::Kind::SaveGame);
                a.save = name;
                out.push_back(std::move(a));
                pop();
            }
        } else if (is("IDC_DELETE_GAME_BUTTON")) {
            const GuiControl* l = d->get("IDC_SAVE_GAME_LIST");
            if (l && l->selected >= 0 && static_cast<size_t>(l->selected) < saves_.size()) {  // N1 / t09: confirm first
                confirm_save_ = saves_[static_cast<size_t>(l->selected)].name;
                quit_action_ = "delete";
                push(kQuit);
            }
        }
    } else if (ascii_iequals(n, kLoad) && d) {
        if (is("IDC_LOAD_CANCEL_BUTTON")) pop();
        else if (is("IDC_LOAD_GAME_BUTTON") || is("IDC_DELETE_GAME_BUTTON")) {
            const GuiControl* l = d->get("IDC_SAVE_GAME_LIST");
            if (l && l->selected >= 0 && static_cast<size_t>(l->selected) < saves_.size()) {
                const std::string& name = saves_[static_cast<size_t>(l->selected)].name;
                if (is("IDC_DELETE_GAME_BUTTON")) {  // N1 / t09: confirm first
                    confirm_save_ = name;
                    quit_action_ = "delete";
                    push(kQuit);
                } else {
                    MenuAction a = act(MenuAction::Kind::LoadGame);
                    a.save = name;
                    out.push_back(std::move(a));
                }
            }
        }
    } else if (ascii_iequals(n, kResConfirm) && res_confirm_) {
        if (is("IDC_DIALOG_BOX_BUTTON_LEFT")) end_resolution_confirm(true, out);
        else if (is("IDC_DIALOG_BOX_BUTTON_RIGHT")) end_resolution_confirm(false, out);
    } else if (ascii_iequals(n, kQuit)) {
        if (is("IDC_DIALOG_BOX_BUTTON_LEFT")) {
            pop();
            if (quit_action_ == "overwrite" || quit_action_ == "delete") {  // N1 / t09: the confirmed save request
                MenuAction a = act(quit_action_ == "overwrite" ? MenuAction::Kind::SaveGame : MenuAction::Kind::DeleteSave);
                a.save = confirm_save_;
                out.push_back(std::move(a));
                if (quit_action_ == "overwrite" && ascii_iequals(top(), kSave)) pop();  // saved: the Save dialog closes
                quit_action_.clear();
            } else {
                out.push_back(act(quit_action_ == "exit" ? MenuAction::Kind::ExitGame : MenuAction::Kind::QuitToMainMenu));
            }
        } else if (is("IDC_DIALOG_BOX_BUTTON_RIGHT")) {
            pop();
        }
    }
}

void FrontEnd::update(std::span<const GuiEvent> events, const InputFrame& raw, std::vector<MenuAction>& out) {
    if (!gui_) return;
    now_ = raw.time;
    if (video_status_stale_) {  // U9: last frame's Apply has reached the swapchain; the line says what is in use now
        video_status_stale_ = false;
        if (GuiDialog* vd = gui_->find(kVideo); vd && p_.video) vd->set_text("IDC_PORT_DISPLAY_INFO", gui_->port_text(p_.video().display_info));
    }
    if (res_confirm_) {  // t12: the countdown runs on the host's wall clock; a vanished dialog or the deadline reverts
        GuiDialog* cd = gui_->find(kResConfirm);
        if (!cd || now_ >= res_deadline_) end_resolution_confirm(false, out);
        else show_confirm_countdown(*cd);
    }
    // Key capture: a focused IDC_EDIT_* of a keyboard dialog takes the next key (Tab / Esc / modifiers excluded).
    if (GuiDialog* d = gui_->top(); d && (keyboard_page(d->name()) >= 0 || is_port_controls(d->name())) && d->focus_index() >= 0) {
        GuiControl& c = (*d)[static_cast<size_t>(d->focus_index())];
        const int32_t slot = c.type == GuiGadgetType::EditBox ? keyboard_edit_slot(c.id()) : -1;
        if (slot >= 0) {
            for (uint32_t k = 1; k < 256; ++k) {
                if (!raw.keys_pressed.test(k) || k == vk::kTab || k == vk::kEscape || k == vk::kShift || k == vk::kControl ||
                    k == vk::kAlt || (k >= 0xA0 && k <= 0xA5))
                    continue;
                HotkeyBinding b;
                b.key = static_cast<int32_t>(k);
                b.mod = raw.keys_down.test(vk::kControl) ? HotkeyMod::Ctrl
                        : raw.keys_down.test(vk::kAlt)   ? HotkeyMod::Alt
                        : raw.keys_down.test(vk::kShift) ? HotkeyMod::Shift
                                                         : HotkeyMod::None;
                keys_work_.bind(static_cast<uint16_t>(slot), b);
                keys_dirty_ = true;
                fill_keyboard(*d);
                break;
            }
        }
    }
    for (const GuiEvent& e : events) {
        switch (e.kind) {
            case GuiEvent::Kind::Clicked: on_click(e, out); break;
            case GuiEvent::Kind::Cancel: on_cancel(e, out); break;
            case GuiEvent::Kind::Checked:
            case GuiEvent::Kind::SelectionChanged:
            case GuiEvent::Kind::DoubleClicked:
            case GuiEvent::Kind::ValueChanged: on_change(e, out); break;
            case GuiEvent::Kind::Submitted:
                if (ascii_iequals(e.dialog, kSave)) {
                    GuiEvent b = e;
                    b.kind = GuiEvent::Kind::Clicked;
                    b.control = "IDC_SAVE_GAME_BUTTON";
                    on_click(b, out);
                }
                break;
            default: break;
        }
    }
    for (const MenuAction& a : out)
        if (a.kind == MenuAction::Kind::ApplyVideo) video_status_stale_ = true;
}

}  // namespace alamo::ui
