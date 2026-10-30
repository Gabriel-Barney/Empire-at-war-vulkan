// alamo_ui_screens_tests (P4 completion, CPU): in-game screens (message boxes, battle end, campaign score, retreat ->
// Retreat command, chat typing -> SendChat, planet info, auto-resolve, map selection), the advanced video options in the
// options flow, the multiplayer screens as UI (multiplayer menu -> LAN lobby -> games / join / password -> staging slots
// -> start, direct connect, connecting, disconnect / kick, vote), the HUD panels (encyclopedia delays and layout, hero
// roster -> HeroSelect, black market -> Production, sabotage -> SabotageTarget, corruption -> CorruptionChoice, close)
// and camera hotkeys (overview toggle, control-group jump). With ALAMO_FOC_DATA: every screen opens on the shipped
// GUIDialogs.rc and every panel component resolves in the shipped CommandBarComponents.xml.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alamo/core/hash.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_info_screens.hpp"
#include "alamo/ui/gui_lobby.hpp"
#include "alamo/ui/mouse_cursor.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/gui_screens.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/hud_panels.hpp"
#include "alamo/ui/hud_abilities.hpp"
#include "alamo/ui/targeting.hpp"
#ifdef ALAMO_UI_HAVE_ABILITIES
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#endif
#ifdef ALAMO_UI_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ui;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

// Trimmed copies of the shipped dialogs (same ids / rectangles).
const char* kRc = R"RC(
IDD_DIALOG_BOX2 DIALOGEX 0, 0, 620, 151
STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LTEXT           "TEXT_CAPTION",IDC_CAPTION,12,12,594,15
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_LEFT,7,112,128,24
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_RIGHT,478,112,128,24
    LTEXT           "USER STRING 1 HERE",IDC_USER_TEXT_STRING_1,12,44,594,8
    LTEXT           "USER STRING 2 HERE",IDC_USER_TEXT_STRING_2,12,62,594,8
    LTEXT           "USER STRING 3 HERE",IDC_USER_TEXT_STRING_3,12,80,594,8
END

IDD_DIALOG_BOX2_CENTERED DIALOGEX 0, 0, 620, 151
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CTEXT           "TEXT_CAPTION",IDC_CAPTION,12,12,594,15
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_LEFT,7,112,128,24
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_RIGHT,478,112,128,24
    CTEXT           "USER STRING 1 HERE",IDC_USER_TEXT_STRING_1,12,44,594,8
    CTEXT           "USER STRING 2 HERE",IDC_USER_TEXT_STRING_2,12,62,594,8
    CTEXT           "USER STRING 3 HERE",IDC_USER_TEXT_STRING_3,12,80,594,8
END

IDD_BATTLE_END_DIALOG DIALOGEX 0, 0, 913, 667
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_CONTINUE",IDC_CONTINUE_BUTTON,522,609,321,24
    LTEXT           "Win lose",IDC_TITLE_STATIC,36,34,838,17
    RTEXT           "battle time",IDC_TIME_STATIC,383,53,484,14
    LTEXT           "Static",IDC_BATTLE_END_PLANET_STATIC,89,63,673,17
    DEFPUSHBUTTON   "TEXT_BUTTON_INVADE",IDC_LAND_INVADE_BUTTON,54,609,187,24
    DEFPUSHBUTTON   "TEXT_BUTTON_SAVE_REPLAY",IDC_SAVE_REPLAY_BUTTON,522,642,321,24
    CTEXT           "",IDC_COUNTDOWN_TIMER,120,612,56,18
    CONTROL         "",IDC_YOUR_UNIT0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,22,266,49,47
    LTEXT           "Static",IDC_YOUR_UNIT_NAME0,75,270,149,18
    CONTROL         "",IDC_YOUR_UNIT1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,22,315,49,47
    LTEXT           "Static",IDC_YOUR_UNIT_NAME1,75,319,149,8
    CONTROL         "",IDC_ENEMY_UNIT0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,470,266,49,47
    LTEXT           "Static",IDC_ENEMY_UNIT_NAME0,523,271,149,8
    CONTROL         "",IDC_YOUR_SCROLLBAR,"msctls_trackbar32",TBS_VERT | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,447,253,15,327
    RTEXT           "bounty",IDC_STATIC_BOUNTY,510,81,359,15
    LTEXT           "TEXT_YOUR_LOSSES",IDC_STATIC_LOSS1,51,147,359,15
    LTEXT           "TEXT_ENEMY_LOSSES",IDC_STATIC_LOSS2,499,147,381,14
    RTEXT           "",IDC_YOUR_LOSS_VAL_STATIC,300,130,100,15
    RTEXT           "",IDC_EFFICIENCY_STATIC,300,170,100,15
END

IDD_RETREAT_DIALOG DIALOGEX 0, 0, 495, 135
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_RETREAT",IDC_RETREAT,328,90,147,20
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_RETRY_CANCEL,20,90,147,20
    DEFPUSHBUTTON   "TEXT_BUTTON_AUTORESOLVE",IDC_AUTORESOLVE,174,90,147,20
    CTEXT           "TEXT_RETREAT_DIALOG",IDC_RETREAT_HEADER,93,24,308,24
END

IDD_INGAME_CHAT_DIALOG DIALOGEX 0, 0, 679, 48
STYLE DS_SETFONT | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "TEXT_TEAM_MEMBERS_ONLY",IDC_TEAM_MEMBERS_ONLY,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,5,28,300,14
    EDITTEXT        IDC_IME_EDITBOX,5,2,655,19,ES_AUTOHSCROLL
END

IDD_SELECT_MAP_DIALOG DIALOGEX 0, 0, 620, 520
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_LOAD_CANCEL_BUTTON,13,477,160,24
    PUSHBUTTON      "TEXT_SELECT_MAP_BUTTON",IDC_LOAD_MAP_BUTTON,447,477,160,24
    LISTBOX         IDC_SAVED_MAPS_LIST,7,68,606,372,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
END

IDD_MULTIPLAYER_DIALOG DIALOGEX 0, 0, 512, 1024
STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_MULTIPLAYER_INTERNET",IDC_MULTIPLAYER_INTERNET_BUTTON,265,526,235,21
    PUSHBUTTON      "TEXT_BUTTON_MULTIPLAYER_LAN",IDC_MULTIPLAYER_LAN_BUTTON,265,562,236,21
    PUSHBUTTON      "TEXT_BUTTON_INTERNET_OPTIONS",IDC_MAIN_OPTIONS_NETWORK,265,598,236,21
    PUSHBUTTON      "TEXT_BUTTON_BACK",IDC_OK_MAIN,265,634,236,21
END

IDD_LAN_LOBBY_DIALOG DIALOGEX 0, 0, 955, 701
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_LAN_CANCEL_BUTTON,730,650,200,24
    PUSHBUTTON      "TEXT_BUTTON_HOST_GAME",IDC_LAN_HOST_GAME_BUTTON,284,353,186,24
    PUSHBUTTON      "TEXT_BUTTON_JOIN_GAME",IDC_LAN_JOIN_GAME_BUTTON,490,353,186,24
    LISTBOX         IDC_LAN_CHAT_LIST,20,394,700,240,LBS_NOINTEGRALHEIGHT | WS_VSCROLL
    EDITTEXT        IDC_LAN_CHAT_EDIT,20,658,674,29,ES_AUTOHSCROLL
    EDITTEXT        IDC_LAN_NAME_EDIT,721,83,208,18,ES_AUTOHSCROLL
    LISTBOX         IDC_PLAYER_LIST,721,130,234,402,LBS_NOINTEGRALHEIGHT | WS_VSCROLL
    COMBOBOX        IDC_LAN_COLOR_SELECT,856,53,73,169,CBS_DROPDOWNLIST | WS_TABSTOP
    LISTBOX         IDC_GAME_LIST2,20,54,700,50,LBS_NOINTEGRALHEIGHT
    LISTBOX         IDC_GAME_LIST,20,84,700,266,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    PUSHBUTTON      "TEXT_INTERNET_PING_BUTTON",IDC_INTERNET_PING_BUTTON,587,54,85,18
END

IDD_LAN_STAGING_DIALOG DIALOGEX 0, 0, 955, 701
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_LAN_CANCEL_BUTTON,10,647,170,24
    PUSHBUTTON      "TEXT_BUTTON_START_GAME",IDC_LAN_START_GAME_BUTTON,746,647,189,24
    LISTBOX         IDC_LAN_CHAT_LIST,10,395,925,197,LBS_NOINTEGRALHEIGHT | WS_VSCROLL
    EDITTEXT        IDC_LAN_CHAT_EDIT,10,605,925,18,ES_AUTOHSCROLL
    LISTBOX         IDC_STAGING_MAP_LIST,221,140,232,197,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    PUSHBUTTON      "TEXT_BUTTON_INTERNET_LOAD",IDC_LOAD_GAME,10,290,201,24
    COMBOBOX        IDC_PLAYER_NAME_1,495,101,155,128,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_NAME_0,495,68,155,128,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_FACTION_1,654,101,115,120,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_FACTION_0,654,68,115,120,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_COLOR_1,773,101,75,216,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_COLOR_0,773,68,75,216,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_TEAM_1,854,101,95,192,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_TEAM_0,854,68,95,192,CBS_DROPDOWNLIST | WS_TABSTOP
    CONTROL         "Custom2",IDC_ACCEPT_PLAYER_0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,476,70,14,14
    CONTROL         "Custom2",IDC_ACCEPT_PLAYER_1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,476,103,14,14
END

IDD_ENTER_GAME_PASSWORD_DIALOG DIALOGEX 0, 0, 494, 163
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_CANCEL_BUTTON,13,127,128,24
    PUSHBUTTON      "TEXT_BUTTON_OK",IDC_OK,353,127,128,24
    EDITTEXT        IDC_INTERNET_GAME_PASSWORD_EDIT,241,64,160,18,ES_AUTOHSCROLL
END

IDD_DISCONNECT_DIALOG DIALOGEX 0, 0, 687, 578
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_DISCONNECT_QUIT",IDC_QUIT_GAME_BUTTON,279,524,128,24
    RTEXT           "Player 0",IDC_PLAYER_NAME_0,20,93,200,12
    CONTROL         "",IDC_CONNECTION_QUALITY0,"msctls_progress32",WS_BORDER,235,93,250,12
    PUSHBUTTON      "TEXT_KICK",IDC_BUTTON_KICK_PLAYER0,498,93,100,12
    RTEXT           "Player 0",IDC_PLAYER_NAME_1,20,119,200,12
    CONTROL         "",IDC_CONNECTION_QUALITY1,"msctls_progress32",WS_BORDER,235,119,250,12
    PUSHBUTTON      "TEXT_KICK",IDC_BUTTON_KICK_PLAYER1,498,119,100,12
END

IDD_MP_VOTE_DIALOG DIALOGEX 0, 0, 415, 286
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_LEFT,10,255,189,24
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_RIGHT,213,255,189,24
    RTEXT           "TEXT_COUNTDOWN",IDC_COUNTDOWN_TIMER,336,32,66,8
    LTEXT           "TEXT_PLAYER_0",IDC_NAME_PLAYER_0,10,76,180,8
    LTEXT           "TEXT_WAITING",IDC_RESULT_PLAYER_0,200,76,108,8
    LTEXT           "Planet name here",IDC_TEXT_MP_VOTE_PLANET_NAME,10,6,392,10
END

IDD_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 570, 454
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_ADVANCED",IDC_ADVANCED,14,330,148,24
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_CANCEL,14,412,148,24
    DEFPUSHBUTTON   "TEXT_BUTTON_ACCEPT",IDC_OK,408,412,148,24
    PUSHBUTTON      "TEXT_BUTTON_APPLY",IDC_APPLY,230,412,148,24
    LISTBOX         IDC_RESOLUTION_LIST,14,60,300,200,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    CONTROL         "",IDC_ANTI_ALIAS_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,256,246,251,14
    CONTROL         "",IDC_DETAIL_LEVEL_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,256,271,251,14
    CONTROL         "",IDC_GAMMA_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,256,296,251,14
    CONTROL         "TEXT_CHECK_VSYNC_ENABLED",IDC_VSYNC_ENABLED,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,22,332,234,10
END

IDD_ADVANCED_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 571, 454
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_CANCEL",IDC_CANCEL,14,412,148,24
    PUSHBUTTON      "TEXT_BUTTON_ACCEPT",IDC_OK,408,412,148,24
    CONTROL         "",IDC_PARTICLE_DETAIL_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,229,75,253,15
    CONTROL         "TEXT_CHECK_BLOOM",IDC_BLOOM,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,9,300,200,10
END

IDD_SKIRMISH_BUY_UNITS_DIALOG DIALOGEX 0, 0, 527, 472
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LISTBOX         IDC_SKIRMISH_BUY_FORCES_LISTBOX,7,84,513,257,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_BACK_BUTTON,11,430,130,30
    LTEXT           "TEXT_SKIRMISH_BUY_UNITS_CREDITS_BALANCE",IDC_SKIRMISH_BUY_UNITS_CREDITS_TEXT,10,341,299,15
    PUSHBUTTON      "TEXT_BUTTON_BUY",IDC_BUY_BUTTON,12,374,160,30
    PUSHBUTTON      "TEXT_BUTTON_SELL",IDC_SELL_BUTTON,183,374,160,30
    PUSHBUTTON      "TEXT_BUTTON_SELL_ALL",IDC_SELL_ALL_BUTTON,353,374,160,30
    PUSHBUTTON      "TEXT_BUTTON_START_GAME",IDC_NEXT_BUTTON,386,430,130,30
    CONTROL         "",IDC_SKIRMISH_BUY_MENU_UNIT_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,400,20,60,60
END

IDD_STORY_DIALOG DIALOGEX 0, 0, 382, 519
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CTEXT           "TXT_STORY_DIALOG_TITLE",IDC_STATIC,11,7,359,24
    LISTBOX         IDC_STORY_TEXT,12,186,360,285,LBS_NOINTEGRALHEIGHT | WS_VSCROLL
    CONTROL         "Custom2",IDC_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,12,35,360,145
    PUSHBUTTON      "TEXT_BUTTON_CLOSE",IDC_STORY_DIALOG_CLOSE_BUTTON,135,480,110,16
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_STORY_DIALOG_CANCEL,258,480,110,16
    PUSHBUTTON      "TEXT_BUTTON_ACCEPT",IDC_STORY_DIALOG_ACCEPT,12,480,110,16
END

IDD_PLANET_INFO DIALOGEX 0, 0, 975, 718
STYLE DS_SETFONT | DS_MODALFRAME | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU
FONT 8, "MS Shell Dlg", 400, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_CLOSE",IDC_CLOSE_BUTTON,575,665,168,33
    LTEXT           "TEXT_TACTICAL_INFORMATION",IDC_STATIC,58,167,366,19
    LISTBOX         IDC_LIST_REFERENCE,51,345,400,275,LBS_SORT | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LISTBOX         IDC_LIST_TACTICAL,51,176,400,69,LBS_SORT | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LISTBOX         IDC_LIST_ADVANTAGE,51,258,400,67,LBS_SORT | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    CONTROL         "",IDC_PLANET_ICON,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,463,166,435,435
    LTEXT           "current planet name",IDC_STATIC_PLANET_NAME,72,63,866,18
    CONTROL         "",IDC_PLANET_OVERLAY,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,442,165,482,472
    CTEXT           "swamp",IDC_STATIC_TERRAIN,688,344,196,61
    LTEXT           "rain",IDC_STATIC_WEATHER,737,250,144,43
    CONTROL         "",IDC_WEATHER_ICON,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,607,234,128,128
    CONTROL         "",IDC_MAP_ICON,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,685,396,200,200
    CONTROL         "",IDC_NATIVE_ICON,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,478,358,209,209
    CTEXT           "human",IDC_STATIC_NATIVE,479,334,207,44
END

IDD_PLANET_DIALOG DIALOGEX 0, 0, 435, 384
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LTEXT           "STUB PLANET NAME GOES HERE",IDC_PLANET_MENU_PLANET_NAME,8,7,237,14
    PUSHBUTTON      "TEXT_BUTTON_ORGANIZATION",IDC_PLANET_ORGANIZATION_BUTTON,294,347,130,30
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_PLANET_EXIT_BUTTON,10,347,130,30
    PUSHBUTTON      "TEXT_BUTTON_PRODUCTION",IDC_PLANET_PRODUCTION_BUTTON,152,347,130,30
    LTEXT           "STUB OBJECT BUILDING",IDC_TEXT_PRODUCTION_OBJECT_BUILDING,34,99,191,12
    CONTROL         "",IDC_PRODUCTION_MENU_PROGRESS,"msctls_progress32",WS_BORDER,34,115,191,10
    CONTROL         "",IDC_POLITICAL_ICON_0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,250,30,20,20
    CONTROL         "",IDC_POLITICAL_ICON_1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,272,30,20,20
    CONTROL         "",IDC_STARBASE_ICON_0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,250,52,20,20
    CONTROL         "",IDC_GROUND_BASE_ICON_0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,250,74,20,20
    RTEXT           "STUB +CREDITS",IDC_TEXT_PLANET_CREDITS,300,7,120,14
END

IDD_DATA_CHART DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "TEXT_DROID_LOG",IDC_DROID_LOG_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,18,10,150,20
    CONTROL         "TEXT_DATA_CHART",IDC_DATA_CHART_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,170,10,150,20
    CONTROL         "TEXT_TECH_TREE",IDC_TECH_TREE_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,322,10,150,20
    CONTROL         "TEXT_SUMMARY_SCREEN",IDC_SUMMARY_SCREEN_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,474,10,150,20
    PUSHBUTTON      "TEXT_BUTTON_CLOSE",IDC_BUTTON_CLOSE,722,563,141,14
    CONTROL         "",IDC_DATA_CHART_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,189,71,470,265
    CONTROL         "TEXT_CHECK_ECONOMY",IDC_RADIO_ECONOMY,"Button",BS_AUTORADIOBUTTON | WS_GROUP,680,80,140,12
    CONTROL         "TEXT_CHECK_MILITARY",IDC_RADIO_MILITARY,"Button",BS_AUTORADIOBUTTON,680,100,140,12
    CONTROL         "TEXT_CHECK_PLANETS",IDC_RADIO_PLANETS,"Button",BS_AUTORADIOBUTTON,680,120,140,12
    RTEXT           "Static",IDC_Y_MAX,27,71,155,8
    LTEXT           "Static",IDC_STATIC_END,667,340,141,10
END

IDD_TECH_TREE_EMPIRE DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "TEXT_DROID_LOG",IDC_DROID_LOG_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,18,10,150,20
    CONTROL         "TEXT_DATA_CHART",IDC_DATA_CHART_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,170,10,150,20
    CONTROL         "TEXT_TECH_TREE",IDC_TECH_TREE_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,322,10,150,20
    CONTROL         "TEXT_SUMMARY_SCREEN",IDC_SUMMARY_SCREEN_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,474,10,150,20
    PUSHBUTTON      "TEXT_BUTTON_CLOSE",IDC_BUTTON_CLOSE,722,563,141,14
    CONTROL         "",IDC_TECHTREE_E_Ground_Barracks,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,100,100,40,40
    CONTROL         "",IDC_TECHTREE_Imperial_Stormtrooper_Squad,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,150,100,40,40
    CONTROL         "",IDC_TECHTREE_Dark_Trooper_PhaseI,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,200,100,40,40
    CONTROL         "TEXT_GAME_TYPE_TEAM_LAND",IDC_TECH_TREE_LAND,"Button",BS_AUTORADIOBUTTON | WS_GROUP,700,40,80,12
    CONTROL         "TEXT_GAME_TYPE_TEAM_SPACE",IDC_TECH_TREE_SPACE,"Button",BS_AUTORADIOBUTTON,790,40,80,12
END

IDD_TECH_TREE_EMPIRE_SPACE DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "TEXT_TECH_TREE",IDC_TECH_TREE_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,322,10,150,20
    PUSHBUTTON      "TEXT_BUTTON_CLOSE",IDC_BUTTON_CLOSE,722,563,141,14
    CONTROL         "",IDC_TECHTREE_Star_Destroyer,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,100,100,40,40
    CONTROL         "TEXT_GAME_TYPE_TEAM_LAND",IDC_TECH_TREE_LAND,"Button",BS_AUTORADIOBUTTON | WS_GROUP,700,40,80,12
    CONTROL         "TEXT_GAME_TYPE_TEAM_SPACE",IDC_TECH_TREE_SPACE,"Button",BS_AUTORADIOBUTTON,790,40,80,12
END

IDD_SUMMARY_SCREEN_PRODUCTION DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "TEXT_DROID_LOG",IDC_DROID_LOG_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,18,10,150,20
    CONTROL         "TEXT_SUMMARY_SCREEN",IDC_SUMMARY_SCREEN_RADIO,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,474,10,150,20
    DEFPUSHBUTTON   "TEXT_BUTTON_CLOSE",IDC_BUTTON_EXIT,722,563,141,14
    DEFPUSHBUTTON   "TEXT_POP_CAP",IDC_BUTTON_POP_CAP,600,60,80,14
    CONTROL         "Custom1",IDC_PLANET_ICON0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,30,90,30,30
    LTEXT           "planet name0",IDC_PLANET_NAME0,64,95,100,10
    CONTROL         "Custom1",IDC_HERO0_ICON0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,300,90,30,30
    RTEXT           "Static",IDC_POP_CAP_0,600,95,60,10
    CONTROL         "Custom1",IDC_PLANET_ICON1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,30,130,30,30
    LTEXT           "planet name1",IDC_PLANET_NAME1,64,135,100,10
    CONTROL         "",IDC_SCROLLBAR_SLIDER,"msctls_trackbar32",TBS_VERT | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,850,80,15,400
END
)RC";

const char* kSkinXml = R"(<GUIDialogs><Textures File="MT_CommandBar"><Default>
  <Frame_Background>i_dialogue_00.tga</Frame_Background>
  <Check_On>i_dialogue_check_on.tga</Check_On>
</Default></Textures><Fonts><Default><Global_Default><Name>Arial</Name><Size>8</Size></Global_Default></Default></Fonts></GUIDialogs>)";

struct Env {
    DialogScript script;
    GuiSkin skin;
    TextureAtlas atlas;
    TextDatabase text;
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    GuiManager gui;
    InputRouter router;
    double now = 10.0;
    uint8_t down = 0;
    Env() {
        std::string err;
        script.parse(kRc, &err);
        const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kSkinXml), "GUIDialogs.xml");
        skin.parse(doc.root());
        atlas.texture_width = atlas.texture_height = 256;
        atlas.add({"i_dialogue_00.tga", 0, 0, 16, 16, true});
        atlas.add({"i_dialogue_check_on.tga", 64, 0, 12, 12, true});
        text.add("TEXT_VICTORY", u"Victory!");
        gui.init(script, skin, atlas, fonts, &text);
        gui.resize(1024, 768);
    }
    // One frame with optional typed text / keys; returns the GUI events.
    std::vector<GuiEvent> frame(Vec2f px, bool left, std::initializer_list<uint16_t> keys = {}, std::u32string_view typed = {}) {
        InputFrame f;
        f.time = now;
        f.width = 1024;
        f.height = 768;
        f.mouse_x = static_cast<int32_t>(px.x);
        f.mouse_y = static_cast<int32_t>(px.y);
        const uint8_t lb = button_bit(Button::Left);
        f.buttons_down = left ? lb : 0;
        f.buttons_pressed = (left && !(down & lb)) ? lb : 0;
        f.buttons_released = (!left && (down & lb)) ? lb : 0;
        down = f.buttons_down;
        for (uint16_t k : keys) {
            f.keys_pressed.set(k);
            f.keys_down.set(k);
        }
        RouteHooks hooks;
        gui.install(hooks);
        RoutedInput ri;
        router.route(f, hooks, ri);
        std::vector<GuiEvent> ev;
        gui.update(f, ri, now, ev, typed);
        now += 0.05;
        return ev;
    }
    Vec2f px_of(const GuiDialog& d, std::string_view id) const {
        const GuiRect r = d.control_rect(static_cast<size_t>(d.find(id)));
        return gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y) - 2.0f);
    }
    std::vector<GuiEvent> click(std::string_view dialog, std::string_view id) {
        const GuiDialog* d = gui.find(dialog);
        if (!d) return {};
        const Vec2f p = px_of(*d, id);
        std::vector<GuiEvent> ev = frame(p, false);
        std::vector<GuiEvent> a = frame(p, true);
        std::vector<GuiEvent> b = frame(p, false);
        ev.insert(ev.end(), a.begin(), a.end());
        ev.insert(ev.end(), b.begin(), b.end());
        return ev;
    }
};

GuiEvent event(GuiEvent::Kind k, std::string_view dialog, std::string_view control, int32_t value = 0) {
    GuiEvent e;
    e.kind = k;
    e.dialog = std::string(dialog);
    e.control = std::string(control);
    e.value = value;
    return e;
}

// ---- in-game screens ------------------------------------------------------------------------------------------
void test_screens() {
    Env env;
    GameScreens gs;
    gs.init(env.gui);
    std::vector<ScreenAction> acts;
    std::vector<commands::EventData> cmds;

    // Message box: 2 buttons -> IDD_DIALOG_BOX2, real click on the right button -> result 1.
    MessageBoxSpec m;
    m.tag = "quit?";
    m.caption = U"Leave the battle?";
    m.lines = {U"Unsaved progress will be lost."};
    m.buttons = {U"Yes", U"No"};
    CHECK(gs.show_message(m));
    CHECK(gs.message_dialog() == "IDD_DIALOG_BOX2");
    const GuiDialog* mb = env.gui.find("IDD_DIALOG_BOX2");
    CHECK(mb && mb->get("IDC_CAPTION")->text == U"Leave the battle?" && mb->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->text == U"No" &&
          mb->get("IDC_USER_TEXT_STRING_1")->visible && !mb->get("IDC_USER_TEXT_STRING_2")->visible);
    gs.update(env.click("IDD_DIALOG_BOX2", "IDC_DIALOG_BOX_BUTTON_RIGHT"), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MessageBoxResult && acts[0].tag == "quit?" && acts[0].button == 1);
    CHECK(env.gui.find("IDD_DIALOG_BOX2") == nullptr && gs.message_dialog().empty());
    // Esc -> -1; a 3-button box missing from this script -> false.
    CHECK(gs.show_message(m));
    acts.clear();
    gs.update(env.frame({5, 5}, false, {vk::kEscape}), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].button == -1);
    m.buttons.push_back(U"Cancel");
    CHECK(!gs.show_message(m));

    // Battle end: title from TEXT_VICTORY, losses with counts, invade hidden, countdown, scrolling, continue.
    BattleEndInfo be;
    be.victory = true;
    be.planet = U"Kuat";
    be.time = U"12:34";
    be.bounty = 1500;
    for (int i = 0; i < 15; ++i) be.your_losses.push_back({"i_button_tie.tga", U"TIE " + std::u32string(1, static_cast<char32_t>(U'A' + static_cast<char32_t>(i))), static_cast<uint32_t>(i % 3 + 1)});
    be.enemy_losses = {{"i_button_xwing.tga", U"X-Wing", 4}};
    be.countdown = 9.2f;
    CHECK(gs.show_battle_end(be));
    GuiDialog* bd = env.gui.find(dlg::kBattleEnd);
    if (bd) {  // QA U2 / U3: names wrap in a box as tall as the icon; without heroes the rows sit right under the header
        const size_t icon0 = static_cast<size_t>(bd->find("IDC_YOUR_UNIT0")), name0 = static_cast<size_t>(bd->find("IDC_YOUR_UNIT_NAME0"));
        const GuiRect ir = bd->control_rect(icon0), nr = bd->control_rect(name0);
        CHECK((*bd)[name0].fit_lines == 2 && std::abs(nr.max.y - ir.max.y) < 0.01f && std::abs(nr.height() - ir.height()) < 0.01f);
        const float top = bd->origin().y - static_cast<float>((*bd)[icon0].def->y);
        CHECK(std::abs(ir.max.y - (top + kBattleEndRowsUp)) < 0.01f);
    }
    {
        BattleEndInfo raw = be;
        raw.planet = U"_Space_Planet_Felucia_01.ted";
        raw.your_heroes = {"i_button_vader.tga"};
        CHECK(gs.show_battle_end(raw));
        GuiDialog* rd = env.gui.find(dlg::kBattleEnd);
        CHECK(rd && rd->get("IDC_BATTLE_END_PLANET_STATIC")->text.empty() && rd->get("IDC_YOUR_UNIT0")->offset.y == 0.0f &&
              rd->get("IDC_ENEMY_UNIT0")->offset.y == kBattleEndRowsUp);
        CHECK(gs.show_battle_end(be));
    }
    CHECK(bd && bd->get("IDC_TITLE_STATIC")->text == U"Victory!" && bd->get("IDC_YOUR_UNIT_NAME0")->text == U"TIE A" &&
          bd->get("IDC_YOUR_UNIT_NAME1")->text == U"TIE B x2" && bd->get("IDC_ENEMY_UNIT_NAME0")->text == U"X-Wing x4" &&
          bd->get("IDC_YOUR_UNIT_NAME0")->fit_keep_tail == 0 && bd->get("IDC_YOUR_UNIT_NAME1")->fit_keep_tail == 3 &&  // t47
          bd->get("IDC_YOUR_UNIT0")->image == "i_button_tie.tga" && !bd->get("IDC_LAND_INVADE_BUTTON")->visible &&
          bd->get("IDC_COUNTDOWN_TIMER")->text == U"10" && bd->get("IDC_STATIC_BOUNTY")->text == U"1500" &&
          bd->get("IDC_YOUR_SCROLLBAR")->visible);
    acts.clear();
    gs.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, dlg::kBattleEnd, "IDC_YOUR_SCROLLBAR", 100)}, acts, cmds);
    CHECK(gs.battle_end_offset(false) == 3 && bd->get("IDC_YOUR_UNIT_NAME0")->text == U"TIE D");
    gs.set_countdown(-1.0f);
    CHECK(!bd->get("IDC_COUNTDOWN_TIMER")->visible);
    gs.update(env.click(dlg::kBattleEnd, "IDC_SAVE_REPLAY_BUTTON"), acts, cmds);
    gs.update(env.click(dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"), acts, cmds);

    // Playtest 2 t13: auto-resolve results figures. Battle time from seconds (TEXT_BATTLE_TIME without a number is followed by
    // hh:mm:ss; a pattern with %02d takes the parts), bounty through TEXT_SUMMARY_BOUNTY_COLLECTED, loss values and
    // efficiencies in their own controls where the template has them (yours) and appended to the loss header where not (enemy).
    {
        BattleEndInfo ar;
        ar.victory = true;
        ar.planet = U"Kuat";
        ar.battle_seconds = 3725;
        ar.bounty = 80;
        ar.your_loss_value = 200;
        ar.enemy_loss_value = 500;
        ar.your_efficiency = 71;
        ar.enemy_efficiency = 29;
        CHECK(gs.show_battle_end(ar));
        GuiDialog* d = env.gui.find(dlg::kBattleEnd);
        CHECK(d && d->get("IDC_TIME_STATIC")->text == U"Battle Time: 01:02:05" && d->get("IDC_STATIC_BOUNTY")->text == U"80");
        CHECK(d->get("IDC_YOUR_LOSS_VAL_STATIC")->text == U"$ 200" && d->get("IDC_EFFICIENCY_STATIC")->text == U"71%");
        CHECK(d->get("IDC_STATIC_LOSS1")->text == U"TEXT_YOUR_LOSSES");  // yours: own controls, header untouched
        CHECK(d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES   $ 500   29%");
        env.text.add("TEXT_BATTLE_TIME", u"Time %02d:%02d:%02d");
        env.text.add("TEXT_SUMMARY_BOUNTY_COLLECTED", u"Salvage collected: $ %d");
        CHECK(gs.show_battle_end(ar));  // shown again without closing: the appended header text is replaced, not stacked
        d = env.gui.find(dlg::kBattleEnd);
        CHECK(d->get("IDC_TIME_STATIC")->text == U"Time 01:02:05" && d->get("IDC_STATIC_BOUNTY")->text == U"Salvage collected: $ 80");
        CHECK(d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES   $ 500   29%");
        ar.battle_seconds = 59;
        ar.time = U"explicit";  // an explicit time text wins
        CHECK(gs.show_battle_end(ar) && env.gui.find(dlg::kBattleEnd)->get("IDC_TIME_STATIC")->text == U"explicit");
        BattleEndInfo plain;  // nothing computed: the old behaviour (bounty hidden, header untouched)
        plain.time = U"12:34";
        CHECK(gs.show_battle_end(plain));
        d = env.gui.find(dlg::kBattleEnd);
        CHECK(!d->get("IDC_STATIC_BOUNTY")->visible && d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES");
        gs.close_all();
    }
    CHECK(acts.size() == 2 && acts[0].kind == ScreenAction::Kind::SaveReplay && acts[1].kind == ScreenAction::Kind::BattleEndContinue);
    CHECK(env.gui.find(dlg::kBattleEnd) == nullptr && cmds.empty());

    // Retreat: the only screen click that becomes a command.
    CHECK(gs.show_retreat_confirm());
    acts.clear();
    gs.update(env.click(dlg::kRetreat, "IDC_RETREAT"), acts, cmds);
    CHECK(cmds.size() == 1 && cmds[0] == commands::ev::retreat(true) && acts.size() == 1 &&
          acts[0].kind == ScreenAction::Kind::RetreatConfirmed);
    CHECK(gs.show_retreat_confirm());
    gs.update(env.click(dlg::kRetreat, "IDC_AUTORESOLVE"), acts, cmds);
    CHECK(acts.size() == 2 && acts[1].kind == ScreenAction::Kind::AutoResolve && cmds.size() == 1);

    // Chat: typed text + Enter -> SendChat (team only checked), Esc -> ChatClosed.
    CHECK(gs.open_chat(true) && gs.chat_open());
    acts.clear();
    gs.update(env.frame({5, 5}, false, {}, U"gg wp"), acts, cmds);
    gs.update(env.frame({5, 5}, false, {vk::kReturn}), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::SendChat && acts[0].text == U"gg wp" && acts[0].team_only);
    CHECK(!gs.chat_open());
    CHECK(gs.open_chat());
    gs.update(env.frame({5, 5}, false, {vk::kEscape}), acts, cmds);
    CHECK(acts.size() == 2 && acts[1].kind == ScreenAction::Kind::ChatClosed);

    // Map selection: double click -> MapSelected.
    const SkirmishMapInfo maps[2] = {{"_SPACE_PLANET_KUAT_01", U"Kuat", 2}, {"_LAND_PLANET_TATOOINE_01", U"Tatooine", 4}};
    CHECK(gs.show_select_map(maps));
    acts.clear();
    gs.update(std::vector<GuiEvent>{event(GuiEvent::Kind::DoubleClicked, dlg::kSelectMap, "IDC_SAVED_MAPS_LIST", 1)}, acts, cmds);
    // the selection is the list control's state (row 0 preselected; the manager selects the row before a double click)
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MapSelected && acts[0].map == "_SPACE_PLANET_KUAT_01");
    acts.clear();
    CHECK(gs.show_select_map(maps));
    env.gui.find(dlg::kSelectMap)->set_selected("IDC_SAVED_MAPS_LIST", 1);
    gs.update(env.click(dlg::kSelectMap, "IDC_LOAD_MAP_BUTTON"), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MapSelected && acts[0].map == "_LAND_PLANET_TATOOINE_01");
    // Screens not in this script fail cleanly.
    CHECK(!gs.show_auto_resolve({}) && !gs.show_campaign_score({}));
    for (int k = 0; k < 14; ++k) std::printf("%s%s", k ? " " : "screen actions: ", screen_action_name(static_cast<ScreenAction::Kind>(k)));
    std::printf("\n");
}

// ---- advanced video options in the options flow -------------------------------------------------------------------
void test_advanced_video() {
    Env env;
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.particle = 40;
    cur.bloom = false;
    p.advanced_video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    env.gui.open("IDD_VIDEO_OPTIONS_DIALOG");
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    GuiDialog* d = env.gui.top();
    CHECK(d && d->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 40 && !d->get("IDC_BLOOM")->checked);
    d->set_value("IDC_PARTICLE_DETAIL_SLIDER", 75);
    d->set_checked("IDC_BLOOM", true);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_OK")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAdvancedVideo && acts[0].advanced.particle == 75 &&
          acts[0].advanced.bloom && acts[0].advanced.mesh == 100);
    CHECK(fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(std::string(menu_action_name(MenuAction::Kind::AutoDetectVideo)) == "AutoDetectVideo");
}

// ---- t12 (playtest 3): "keep these settings?" after a resolution change ------------------------------------------
void test_resolution_confirm() {
    Env env;
    env.text.add("TEXT_RESOLUTION_CHANGED_CONFIRMATION", u"Your screen resolution has been changed.  Do you want to keep these settings?");
    env.text.add("TEXT_REVERTING_IN_X_SECONDS", u"Reverting in %d seconds.");
    env.text.add("TEXT_BUTTON_YES", u"Yes");
    env.text.add("TEXT_BUTTON_NO", u"No");
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;  // what the host reports: the mode in use
    cur.resolutions = {U"800 x 600 (60Hz)", U"1280 x 720 (60Hz)", U"1920 x 1080 (60Hz)", U"2560 x 1440 (144Hz)"};
    cur.resolution = 1;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    InputFrame raw;
    raw.time = 100.0;
    auto open_video = [&] {
        env.gui.close_all();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, raw, acts);
        return env.gui.top();
    };
    auto click_video = [&](const char* id, int32_t pick) {
        GuiDialog* v = env.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
        if (v) v->set_selected("IDC_RESOLUTION_LIST", pick);
        acts.clear();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", id)}, raw, acts);
    };
    auto line2 = [&]() -> std::u32string {
        const GuiDialog* c = env.gui.find("IDD_DIALOG_BOX2_CENTERED");
        return c ? c->get("IDC_USER_TEXT_STRING_2")->text : std::u32string();
    };

    GuiDialog* d = open_video();
    CHECK(fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" && d && d->get("IDC_RESOLUTION_LIST")->selected == 1);
    // Applying the mode already in use asks nothing.
    click_video("IDC_APPLY", 1);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && !acts[0].video.unconfirmed && !fe.resolution_confirm_open());

    // A new resolution: applied tentatively, the box opens with the sentence, the buttons and a 15 s countdown.
    click_video("IDC_APPLY", 3);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 3 && acts[0].video.unconfirmed);
    CHECK(fe.resolution_confirm_open() && fe.top() == "IDD_DIALOG_BOX2_CENTERED");
    const GuiDialog* c = env.gui.top();
    CHECK(c->get("IDC_USER_TEXT_STRING_1")->text == U"Your screen resolution has been changed.  Do you want to keep these settings?" &&
          c->get("IDC_DIALOG_BOX_BUTTON_LEFT")->text == U"Yes" && c->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->text == U"No" &&
          !c->get("IDC_USER_TEXT_STRING_3")->visible);
    CHECK(line2() == U"Reverting in 15 seconds." && std::fabs(fe.resolution_confirm_remaining() - kResolutionConfirmSeconds) < 1e-9);
    // The countdown follows the frame clock and sends nothing while it runs.
    acts.clear();
    raw.time = 105.2;
    fe.update({}, raw, acts);
    CHECK(acts.empty() && line2() == U"Reverting in 10 seconds." && fe.resolution_confirm_open());
    raw.time = 114.9;
    fe.update({}, raw, acts);
    CHECK(acts.empty() && line2() == U"Reverting in 1 seconds.");
    // Timeout: the previous resolution is applied again (confirmed, so the host stores it); the Apply dialog shows it selected.
    raw.time = 115.0;
    fe.update({}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && acts[0].video.resolution == 1 && !acts[0].video.unconfirmed);
    CHECK(!fe.resolution_confirm_open() && fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" &&
          env.gui.top()->get("IDC_RESOLUTION_LIST")->selected == 1);

    // Yes keeps: the same settings again, confirmed; the dialog closes and stays closed.
    raw.time = 200.0;
    click_video("IDC_APPLY", 2);
    CHECK(acts.size() == 1 && acts[0].video.unconfirmed && fe.resolution_confirm_open());
    acts.clear();
    raw.time = 201.0;
    fe.update(env.click("IDD_DIALOG_BOX2_CENTERED", "IDC_DIALOG_BOX_BUTTON_LEFT"), raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    cur.resolution = 2;  // the host now reports the kept mode
    acts.clear();
    raw.time = 230.0;
    fe.update({}, raw, acts);
    CHECK(acts.empty());  // the old deadline is gone

    // No reverts at once. OK closes the video dialog first; the box then sits over the options.
    open_video();
    raw.time = 300.0;
    click_video("IDC_OK", 0);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 0 && acts[0].video.unconfirmed && fe.top() == "IDD_DIALOG_BOX2_CENTERED" &&
          env.gui.find("IDD_VIDEO_OPTIONS_DIALOG") == nullptr);
    acts.clear();
    raw.time = 302.0;
    fe.update(env.click("IDD_DIALOG_BOX2_CENTERED", "IDC_DIALOG_BOX_BUTTON_RIGHT"), raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          env.gui.find("IDD_DIALOG_BOX2_CENTERED") == nullptr);

    // Esc is No.
    open_video();
    raw.time = 400.0;
    click_video("IDC_APPLY", 3);
    CHECK(fe.resolution_confirm_open());
    acts.clear();
    raw.time = 401.0;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Cancel, "IDD_DIALOG_BOX2_CENTERED", "")}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    // Other settings travel with the answer: only the resolution is rolled back.
    cur.vsync = true;
    raw.time = 500.0;
    GuiDialog* v = env.gui.top();
    v->set_checked("IDC_VSYNC_ENABLED", true);
    click_video("IDC_APPLY", 0);
    CHECK(acts.size() == 1 && acts[0].video.vsync);
    acts.clear();
    raw.time = 600.0;
    fe.update({}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && acts[0].video.vsync);
}

// ---- t47: the captions stop at the value column; port strings take the pseudo-localization path ----------------------
void test_video_port_text() {
    Env env;
    env.text.pseudo_localize_all();
    CHECK(env.script.extend(kPortDialogExtensionsRc));
    CHECK(env.script.extend(R"RC(
IDD_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 570, 454
BEGIN
    LTEXT           "TEXT_ANTI_ALIASING",IDC_ANTI_ALIAS_STATIC,41,245,146,21
    LTEXT           "TEXT_DETAIL_LEVEL",IDC_STATIC,41,271,146,21
    LTEXT           "TEXT_OFF",IDC_ANTI_ALIAS_OFF_STATIC,190,246,63,21
END
)RC"));
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    cur.display_info = U"160 Hz";
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d != nullptr);
    if (!d) return;
    auto pseudo = [](const char32_t* s) {
        std::u16string u;
        for (; *s; ++s) u.push_back(static_cast<char16_t>(*s));
        return utf16_to_utf32(pseudo_localize(u));
    };
    CHECK(d->get("IDC_PORT_VSYNC_MODE_TEXT")->text == pseudo(U"V-Sync Mode") && d->get("IDC_PORT_FRAME_CAP_TEXT")->text == pseudo(U"Frame Rate Cap"));
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->items[1] == pseudo(U"Standard (FIFO)") && d->get("IDC_PORT_FRAME_CAP")->items[1] == pseudo(U"Display refresh rate"));
    CHECK(d->get("IDC_PORT_DISPLAY_INFO")->text == pseudo(U"160 Hz"));
    const auto rect = [&](const GuiControl* c) { return d->control_rect(static_cast<size_t>(c - &(*d)[0])); };
    const float value_left = rect(d->get("IDC_PORT_ANTI_ALIAS_VALUE")).min.x;
    CHECK(rect(d->get("IDC_ANTI_ALIAS_STATIC")).max.x <= value_left && rect(d->get("IDC_ANTI_ALIAS_STATIC")).min.x < value_left);
    CHECK(d->get("IDC_ANTI_ALIAS_OFF_STATIC")->scale.x == 1.0f);  // not a caption under the value
    bool detail_seen = false;
    for (size_t i = 0; i < d->size(); ++i)
        if ((*d)[i].def && (*d)[i].def->y == 271 && (*d)[i].def->x == 41) {
            detail_seen = true;
            CHECK(d->control_rect(i).max.x <= rect(d->get("IDC_PORT_DETAIL_VALUE")).min.x);
        }
    CHECK(detail_seen);
}

// ---- [port: Gabriel] V-sync mode combo and display line added to the video dialog ----------------------------------
void test_video_vsync_mode() {
    Env env;
    const size_t before = env.script.find("IDD_VIDEO_OPTIONS_DIALOG")->controls.size();
    CHECK(env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    const RcDialog* rd = env.script.find("IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(rd && rd->controls.size() == before + 8 && rd->find("IDC_ADVANCED") >= 0 && rd->find("IDC_PORT_VSYNC_MODE") >= 0 &&
          rd->w == 570 && rd->has_style("DS_CENTER"));  // the shipped header is kept
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    cur.vsync_mode = VsyncMode::Mailbox;
    cur.display_info = U"160 Hz, Mailbox";
    cur.frame_cap = 144;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d && d->get("IDC_PORT_VSYNC_MODE")->selected == 2 && d->get("IDC_PORT_VSYNC_MODE")->items.size() == kVsyncModeCount + 1 &&
          d->get("IDC_PORT_DISPLAY_INFO")->text == U"160 Hz, Mailbox" && d->get("IDC_PORT_FRAME_CAP")->selected == 6 &&
          d->get("IDC_PORT_FRAME_CAP")->items[1] == U"Display refresh rate" && d->get("IDC_PORT_FRAME_CAP")->items[3] == U"60 fps");
    if (!d) return;
    // U9: the check box and the mode list are one setting, whichever is used.
    auto changed = [&](GuiEvent::Kind k, const char* id, int32_t value) {
        GuiEvent e = event(k, "IDD_VIDEO_OPTIONS_DIALOG", id);
        e.value = value;
        fe.update(std::vector<GuiEvent>{e}, InputFrame{}, acts);
    };
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->items[0] == U"Off (no v-sync)" && d->get("IDC_VSYNC_ENABLED")->checked);
    d->set_checked("IDC_VSYNC_ENABLED", false);
    changed(GuiEvent::Kind::Checked, "IDC_VSYNC_ENABLED", 0);
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->selected == 0);
    d->set_checked("IDC_VSYNC_ENABLED", true);
    changed(GuiEvent::Kind::Checked, "IDC_VSYNC_ENABLED", 1);
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->selected == 2);  // back to the remembered Mailbox
    d->set_selected("IDC_PORT_VSYNC_MODE", 0);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 0);
    CHECK(!d->get("IDC_VSYNC_ENABLED")->checked);
    d->set_selected("IDC_PORT_VSYNC_MODE", 1);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 1);
    CHECK(d->get("IDC_VSYNC_ENABLED")->checked && acts.empty());
    // U8: the value beside each slider follows it; the shipped end captions stay.
    CHECK(d->get("IDC_PORT_DETAIL_VALUE")->text == U"100" && d->get("IDC_PORT_GAMMA_VALUE")->text == U"50" && d->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"0");
    changed(GuiEvent::Kind::ValueChanged, "IDC_DETAIL_LEVEL_SLIDER", 25);
    changed(GuiEvent::Kind::ValueChanged, "IDC_GAMMA_SLIDER", 80);
    changed(GuiEvent::Kind::ValueChanged, "IDC_ANTI_ALIAS_SLIDER", 40);
    CHECK(d->get("IDC_PORT_DETAIL_VALUE")->text == U"25" && d->get("IDC_PORT_GAMMA_VALUE")->text == U"80" && d->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"40");
    // Apply leaves the dialog open: the status line picks up the mode the host switched to on the next frame.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Fifo);
    cur.display_info = U"160 Hz, FIFO";
    acts.clear();
    fe.update({}, InputFrame{}, acts);
    CHECK(acts.empty() && d->get("IDC_PORT_DISPLAY_INFO")->text == U"160 Hz, FIFO");
    // Off: v-sync goes off and the mode stays remembered.
    d->set_selected("IDC_PORT_VSYNC_MODE", 0);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 0);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && !acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Mailbox);
    acts.clear();
    d->set_selected("IDC_PORT_VSYNC_MODE", 1);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 1);
    d->set_selected("IDC_PORT_FRAME_CAP", 1);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_OK")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Fifo &&
          acts[0].video.frame_cap == kFrameCapDisplay);
}

// ---- QA U10 / D8 / D10: list scrolling, the port combos' font, one check box size, the port dialogs' layout ---------
void test_options_polish() {
    Env env;
    CHECK(env.script.merge(kPortDialogsRc) && env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    for (int32_t i = 0; i < 40; ++i) cur.resolutions.push_back(std::u32string(U"mode ") + static_cast<char32_t>(0x61 + i % 26));
    cur.resolution = 38;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* v = env.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(v != nullptr);
    if (!v) return;
    // A mode at the end of the list opens with a full list, not with empty rows under the last mode, and is visible.
    const GuiControl* list = v->get("IDC_RESOLUTION_LIST");
    CHECK(list->top > 0 && list->top <= list->selected && list->top < 38 - 3);
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", 1000) && list->top < 40 - 1 && list->top > 0);
    const int32_t last_top = list->top;
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", -5) && list->top == 0);
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", last_top - 1) && list->top == last_top - 1);
    CHECK(!env.gui.scroll_list_to(*v, "IDC_ADVANCED", 1));
    // The combos the port adds take the caption font; a shipped combo would keep the skin's.
    CHECK(v->get("IDC_PORT_VSYNC_MODE")->ui_font && v->get("IDC_PORT_FRAME_CAP")->ui_font);
    CHECK(env.gui.control_font(*v, static_cast<size_t>(v->find("IDC_PORT_VSYNC_MODE"))) ==
          env.gui.control_font(*v, static_cast<size_t>(v->find("IDC_PORT_VSYNC_MODE_TEXT"))));
    // Advanced: check rows run up to the combo column, captions clear of their combos, Title Case.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    GuiDialog* a = env.gui.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    CHECK(a != nullptr);
    if (!a) return;
    for (const char* id : {"IDC_BLOOM", "IDC_PORT_AMBIENT_OCCLUSION"}) {  // the checks this test script has
        const int32_t k = a->find(id);
        CHECK(k >= 0);
        if (k >= 0) CHECK(std::fabs(a->control_rect(static_cast<size_t>(k)).width() - 215.0f) < 0.01f);
    }
    CHECK(a->get("IDC_PORT_AMBIENT_OCCLUSION")->text == U"Ambient Occlusion");
    CHECK(a->get("IDC_PORT_ANTI_ALIASING")->ui_font && a->get("IDC_PORT_ANISOTROPY")->ui_font);
    for (const auto& [caption, combo] : {std::pair{"IDC_PORT_ANTI_ALIASING_TEXT", "IDC_PORT_ANTI_ALIASING"}, std::pair{"IDC_PORT_ANISOTROPY_TEXT", "IDC_PORT_ANISOTROPY"}}) {
        const GuiRect cap = a->control_rect(static_cast<size_t>(a->find(caption)));
        const GuiRect box = a->control_rect(static_cast<size_t>(a->find(combo)));
        CHECK(cap.min.y >= box.max.y && cap.height() == 18.0f);
    }
    // Camera and Controls: the shipped title control (gold header font), both key columns the same width.
    env.gui.close_all();
    const GuiDialog* c = env.gui.open(kPortControlsDialog);
    CHECK(c != nullptr);
    if (!c) return;
    const GuiControl* title = c->get("IDC_STATIC_MEDIUM");
    CHECK(title && title->text == U"Camera and Controls");
    const auto column = [&](const char* id) { return c->control_rect(static_cast<size_t>(c->find(id))); };
    CHECK(column("IDC_EDIT_PORT_PAUSE").width() == column("IDC_EDIT_PORT_SCROLL_UP").width() &&
          column("IDC_EDIT_PORT_PAUSE").min.x == column("IDC_EDIT_PORT_SCROLL_UP").min.x);
}

// ---- [port: Gabriel] anti-aliasing / anisotropic filtering combos and ambient occlusion check (advanced video) ------
void test_advanced_video_port() {
    Env env;
    const size_t before = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG")->controls.size();
    CHECK(env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    const RcDialog* rd = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    CHECK(rd && rd->controls.size() == before + 5 && rd->find("IDC_BLOOM") >= 0 && rd->find("IDC_PORT_AMBIENT_OCCLUSION") >= 0);
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.anti_aliasing = AntiAliasing::Taa;
    cur.anisotropy = 8;
    p.advanced_video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    GuiDialog* video = env.gui.open("IDD_VIDEO_OPTIONS_DIALOG");
    if (!video) return;
    video->set_value("IDC_ANTI_ALIAS_SLIDER", 40);  // t19 / D9: the combo follows the slider
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d && d->get("IDC_PORT_ANTI_ALIASING")->selected == 1 && d->get("IDC_PORT_ANISOTROPY")->selected == 3 &&
          d->get("IDC_PORT_ANISOTROPY")->items.size() == std::size(kAnisotropyChoices) &&
          d->get("IDC_PORT_ANISOTROPY")->items[4] == U"16x" && !d->get("IDC_PORT_AMBIENT_OCCLUSION")->checked);
    if (!d) return;
    d->set_selected("IDC_PORT_ANTI_ALIASING", 0);
    d->set_selected("IDC_PORT_ANISOTROPY", 4);
    d->set_checked("IDC_PORT_AMBIENT_OCCLUSION", true);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAdvancedVideo &&
          acts[0].advanced.anti_aliasing == AntiAliasing::Off && acts[0].advanced.anisotropy == 16 &&
          acts[0].advanced.ambient_occlusion && acts[0].advanced.bloom);
    // t19 / D9: Apply in Advanced moves the slider underneath, both ways (Off -> 0, TAA from 0 -> the default position).
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == 0 && video->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"0");
    d->set_selected("IDC_PORT_ANTI_ALIASING", 1);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == kAntiAliasDefaultSlider && video->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"63");
    // A slider already above 0 keeps its strength.
    video->set_value("IDC_ANTI_ALIAS_SLIDER", 20);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == 20);
    CHECK(anti_aliasing_from_slider(0) == AntiAliasing::Off && anti_aliasing_from_slider(1) == AntiAliasing::Taa);
    // t43: Detail and the seven agree both ways. Apply in Advanced moves the Detail slider to their level...
    d->set_value("IDC_PARTICLE_DETAIL_SLIDER", 30);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.back().advanced.particle == 30 && video->get("IDC_DETAIL_LEVEL_SLIDER")->value == 90 &&
          video->get("IDC_PORT_DETAIL_VALUE")->text == U"90");
    // ... and a Detail slider moved in Video, not yet applied, opens Advanced as its preset; unmoved keeps a custom mix.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_CANCEL")}, InputFrame{}, acts);
    VideoSettings applied;
    applied.detail = 90;
    MenuProviders p2 = p;
    p2.video = [&] { return applied; };
    cur.particle = 30;
    FrontEnd fe2;
    fe2.init(env.gui, {}, p2);
    video->set_value("IDC_DETAIL_LEVEL_SLIDER", 90);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(env.gui.top() && env.gui.top()->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 30);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_CANCEL")}, InputFrame{}, acts);
    video->set_value("IDC_DETAIL_LEVEL_SLIDER", 45);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(env.gui.top() && env.gui.top()->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 45);
    constexpr AdvancedVideoSettings all100;
    CHECK(advanced_detail_level(all100) == 100);
}

// ---- multiplayer screens (UI only) --------------------------------------------------------------------------------
void test_lobby() {
    Env env;
    LobbyScreens lob;
    lob.init(env.gui);
    std::vector<LobbyEvent> ev;
    lob.show_multiplayer_menu();
    CHECK(lob.top() == dlg::kMultiplayer);
    lob.update(env.click(dlg::kMultiplayer, "IDC_MULTIPLAYER_LAN_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::EnterLanLobby && lob.top() == dlg::kLanLobby);
    // Network layer pushes games / players / colours / chat.
    lob.set_local_name(U"Gabriel");
    lob.set_colors({U"Red", U"Blue"}, 1);
    lob.set_games({{"g1", U"Fleet battle", U"Kuat", U"Space", 1, 2, 30, false}, {"g2", U"Locked", U"Hoth", U"Land", 3, 4, -1, true}});
    lob.set_players({{U"Gabriel", 1}, {U"Pilot", 0}});
    for (int i = 0; i < 205; ++i) lob.add_chat(U"line");
    GuiDialog* d = env.gui.find(dlg::kLanLobby);
    CHECK(d && d->get("IDC_GAME_LIST")->items.size() == 2 && d->get("IDC_GAME_LIST")->items[0] == U"Fleet battle  Space  1/2  Kuat  30 ms" &&
          d->get("IDC_GAME_LIST")->items[1].back() == U'*' && d->get("IDC_PLAYER_LIST")->items.size() == 2 &&
          d->get("IDC_LAN_CHAT_LIST")->items.size() == kLobbyChatLines && d->get("IDC_LAN_CHAT_LIST")->top > 0 &&
          d->get("IDC_LAN_NAME_EDIT")->text == U"Gabriel" && d->get("IDC_LAN_COLOR_SELECT")->selected == 1 &&
          !d->get("IDC_GAME_LIST2")->visible && !d->get("IDC_LAN_JOIN_GAME_BUTTON")->enabled);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanLobby, "IDC_GAME_LIST", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanLobby, "IDC_LAN_COLOR_SELECT", 0),
                                     event(GuiEvent::Kind::Clicked, dlg::kLanLobby, "IDC_INTERNET_PING_BUTTON")},
               ev);
    CHECK(ev.size() == 3 && ev[0].kind == LobbyEvent::Kind::SelectGame && ev[0].game == "g1" && ev[1].kind == LobbyEvent::Kind::SetPlayerColor &&
          ev[1].value == 0 && ev[2].kind == LobbyEvent::Kind::SortGames && ev[2].value == 4);
    // Chat: type + Enter in the chat edit (real input path).
    d->focus("IDC_LAN_CHAT_EDIT");
    ev.clear();
    lob.update(env.frame({5, 5}, false, {}, U"hello"), ev);
    lob.update(env.frame({5, 5}, false, {vk::kReturn}), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::Chat && ev[0].text == U"hello" && d->get("IDC_LAN_CHAT_EDIT")->text.empty());
    // Join a password game -> prompt -> JoinGame with the password.
    d->set_selected("IDC_GAME_LIST", 1);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::DoubleClicked, dlg::kLanLobby, "IDC_GAME_LIST", 1)}, ev);
    CHECK(ev.empty() && env.gui.find(dlg::kPassword) != nullptr);
    env.gui.find(dlg::kPassword)->set_text("IDC_INTERNET_GAME_PASSWORD_EDIT", U"sesame");
    lob.update(env.click(dlg::kPassword, "IDC_OK"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::JoinGame && ev[0].game == "g2" && ev[0].text == U"sesame" &&
          env.gui.find(dlg::kPassword) == nullptr);
    // Host -> the network layer answers with the staging state.
    ev.clear();
    lob.update(env.click(dlg::kLanLobby, "IDC_LAN_HOST_GAME_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::HostGame);
    StagingState st;
    st.host = true;
    st.factions = {U"Rebel", U"Empire", U"Underworld"};
    st.colors = {U"Red", U"Blue"};
    st.color_values = {{255, 0, 0, 255}, {0, 0, 255, 255}};
    st.teams = {U"1", U"2"};
    st.maps = {U"Kuat", U"Hoth"};
    st.map = 0;
    StagingSlot me;
    me.state = SlotState::Human;
    me.name = U"Gabriel";
    me.local = true;
    me.faction = 1;
    StagingSlot ai;
    ai.state = SlotState::AI;
    ai.faction = 2;
    st.slots = {me, ai};
    st.options = {{U"Credits", U"10000"}, {U"Pop cap", U"20"}};
    CHECK(lob.show_staging(st));
    GuiDialog* s = env.gui.find(dlg::kLanStaging);
    CHECK(s && lob.top() == dlg::kLanStaging && s->get("IDC_PLAYER_NAME_0")->items[0] == U"Gabriel" && !s->get("IDC_PLAYER_NAME_0")->enabled &&
          s->get("IDC_PLAYER_NAME_1")->selected == 2 && s->get("IDC_PLAYER_NAME_1")->enabled && s->get("IDC_PLAYER_FACTION_1")->selected == 2 &&
          s->get("IDC_PLAYER_FACTION_0")->enabled && s->get("IDC_LAN_START_GAME_BUTTON")->enabled &&
          s->get("IDC_STAGING_MAP_LIST")->selected == 0 && s->get("IDC_ACCEPT_PLAYER_0")->image == "i_dialogue_check_on.tga" &&
          s->get("IDC_PLAYER_COLOR_0")->item_colors.size() == 2);
    {  // the colour combo draws a swatch (no glyphs) for its selected item
        HudDrawList sdl;
        env.gui.build(sdl, env.now);
        bool red = false;
        for (uint32_t layer = 0; layer < kHudLayers; ++layer)
            for (const HudQuad& q : sdl.items(layer, HudList::Alpha)) red = red || (q.color == Rgba8{255, 0, 0, 255});
        CHECK(red);
    }
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_FACTION_1", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_NAME_1", 1),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_TEAM_0", 1),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_STAGING_MAP_LIST", 1)},
               ev);
    CHECK(ev.size() == 4 && ev[0].kind == LobbyEvent::Kind::SetSlotFaction && ev[0].slot == 1 && ev[0].value == 0 &&
          ev[1].kind == LobbyEvent::Kind::SetSlotState && ev[1].slot == 1 && ev[1].value == static_cast<int32_t>(SlotState::Closed) &&
          ev[2].kind == LobbyEvent::Kind::SetSlotTeam && ev[2].slot == 0 && ev[3].kind == LobbyEvent::Kind::SelectMap && ev[3].value == 1);
    ev.clear();
    lob.update(env.click(dlg::kLanStaging, "IDC_LAN_START_GAME_BUTTON"), ev);
    lob.update(env.click(dlg::kLanStaging, "IDC_LOAD_GAME"), ev);
    CHECK(ev.size() == 2 && ev[0].kind == LobbyEvent::Kind::StartGame && ev[1].kind == LobbyEvent::Kind::LoadGame);
    // A client: own slot only; another human not yet accepted blocks the host's start.
    st.host = false;
    st.slots[0].local = false;
    st.slots[1].state = SlotState::Human;
    st.slots[1].name = U"Me";
    st.slots[1].local = true;
    lob.set_staging(st);
    CHECK(!s->get("IDC_LAN_START_GAME_BUTTON")->visible && !s->get("IDC_PLAYER_FACTION_0")->enabled && s->get("IDC_PLAYER_FACTION_1")->enabled);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_FACTION_0", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_STAGING_MAP_LIST", 0)},
               ev);
    CHECK(ev.empty());
    st.host = true;
    st.slots[0].local = true;
    st.slots[1].local = false;
    st.slots[1].accepted = false;
    lob.set_staging(st);
    CHECK(s->get("IDC_LAN_START_GAME_BUTTON")->visible && !s->get("IDC_LAN_START_GAME_BUTTON")->enabled);
    // Esc leaves the game -> back to the LAN lobby.
    ev.clear();
    lob.update(env.frame({5, 5}, false, {vk::kEscape}), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::LeaveGame && lob.top() == dlg::kLanLobby);
    // In-game: disconnect (kick) and vote.
    const ConnectionRow rows[2] = {{U"Host", 1.0f, false}, {U"Lagger", 0.25f, true}};
    lob.show_disconnect(rows);
    GuiDialog* dc = env.gui.find(dlg::kDisconnect);
    CHECK(dc && !dc->get("IDC_BUTTON_KICK_PLAYER0")->visible && dc->get("IDC_BUTTON_KICK_PLAYER1")->visible &&
          dc->get("IDC_CONNECTION_QUALITY1")->value == 25 && dc->get("IDC_PLAYER_NAME_1")->text == U"Lagger");
    ev.clear();
    lob.update(env.click(dlg::kDisconnect, "IDC_BUTTON_KICK_PLAYER1"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::KickPlayer && ev[0].slot == 1);
    env.gui.close(dlg::kDisconnect);
    VoteState v;
    v.planet = U"Coruscant";
    v.names = {U"Gabriel"};
    v.results = {U"Waiting"};
    v.countdown = 4.5f;
    lob.show_vote(v);
    GuiDialog* vd = env.gui.find(dlg::kVote);
    CHECK(vd && vd->get("IDC_TEXT_MP_VOTE_PLANET_NAME")->text == U"Coruscant" && vd->get("IDC_COUNTDOWN_TIMER")->text == U"5" &&
          vd->get("IDC_DIALOG_BOX_BUTTON_LEFT")->text == U"Yes");
    ev.clear();
    lob.update(env.click(dlg::kVote, "IDC_DIALOG_BOX_BUTTON_LEFT"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::Vote && ev[0].value == 1 && !vd->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->enabled);
    env.gui.close(dlg::kVote);
    // Back from the lobby, then from the multiplayer menu.
    ev.clear();
    lob.update(env.click(dlg::kLanLobby, "IDC_LAN_CANCEL_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::LeaveLobby && lob.top() == dlg::kMultiplayer);
    lob.update(env.click(dlg::kMultiplayer, "IDC_OK_MAIN"), ev);
    CHECK(ev.size() == 2 && ev[1].kind == LobbyEvent::Kind::BackToMainMenu && env.gui.open_count() == 0);
    CHECK(std::string(lobby_event_name(LobbyEvent::Kind::SelectClan)) == "SelectClan");
}

// ---- HUD panels ------------------------------------------------------------------------------------------------
const char* kPanelXml = R"(<CommandBarComponents>
  <CommandBarComponent Name="encyclopedia_back"><Type>TextButton</Type><Blank_Texture_Name>e_background.tga</Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_tooltip_rebel_frame.tga i_tooltip_empire_frame.tga</Icon_Alternate_Texture_Name><No_Shell>True</No_Shell>
    <Base_Layer>9</Base_Layer><Color>255 255 255 128</Color><Size>262 14</Size><Offset>5 2</Offset></CommandBarComponent>
  <CommandBarComponent Name="encyclopedia_header_text"><Type>TextButton</Type><Font_Name>Arial Bold</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <Left_Justified>True</Left_Justified><Size>23 18</Size></CommandBarComponent>
  <CommandBarComponent Name="encyclopedia_text"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <Text_Color>192 192 192 200</Text_Color><Left_Justified>True</Left_Justified></CommandBarComponent>
  <CommandBarComponent Name="encyclopedia_right_text"><Type>TextButton</Type><Right_Justified>True</Right_Justified></CommandBarComponent>
  <CommandBarComponent Name="encyclopedia_icon"><Type>TextButton</Type><Size>0.75 0.66</Size></CommandBarComponent>
  <CommandBarComponent Name="zoomed_back"><Type>TextButton</Type><Blank_Texture_Name>i_tooltip_back.tga</Blank_Texture_Name>
    <Base_Layer>4</Base_Layer><Size>262 14</Size><Default_Offset>-132 30</Default_Offset><Offset>5 2</Offset></CommandBarComponent>
  <CommandBarComponent Name="good_hero_frame"><Model_Name>i_main_hero_frame_rebel.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="evil_hero_frame"><Model_Name>i_main_hero_frame_empire.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="hero_slot_good00"><Type>TextButton</Type><Blank_Texture_Name>rebel_hero_ring.tga</Blank_Texture_Name><Base_Layer>3</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="hero_slot_good01"><Type>TextButton</Type><Blank_Texture_Name>rebel_hero_ring.tga</Blank_Texture_Name><Base_Layer>3</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="hero_hb_good00"><Type>Bar</Type><Bar_Overlay_Name>i_bar_health_small10.tga</Bar_Overlay_Name><Smooth_Bar>True</Smooth_Bar><Base_Layer>4</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="hero_slot_evil00"><Type>TextButton</Type><Blank_Texture_Name>rebel_hero_ring.tga</Blank_Texture_Name><Base_Layer>3</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_frame"><Model_Name>i_black_market.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="market_title00"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>10</Font_Point_Size><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_title02"><Type>TextButton</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_title03"><Type>TextButton</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_title04"><Type>TextButton</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_title05"><Type>TextButton</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_00"><Type>Button</Type><Mouse_Over_Texture_Name>i_black_market_selected.tga</Mouse_Over_Texture_Name><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_01"><Type>Button</Type><Mouse_Over_Texture_Name>i_black_market_selected.tga</Mouse_Over_Texture_Name><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_tech_level00"><Type>Button</Type><Icon_Alternate_Texture_Name>i_button_r_tech_upgrade_5.tga i_button_e_tech_upgrade_5.tga</Icon_Alternate_Texture_Name><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="market_close"><Type>TextButton</Type><Icon_Texture_Name>back_button.tga</Icon_Texture_Name><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="bm_frame"><Model_Name>i_sabotage.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="bm_0000"><Type>Button</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="bm_0001"><Type>Button</Type><Base_Layer>7</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="bm_title_402"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="bm_close"><Type>TextButton</Type><Icon_Texture_Name>back_button.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="corruption_shell"><Model_Name>i_corrupt_choice.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="corrupt_button_icon_a"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="corrupt_button_icon_b"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="corrupt_title_a"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="corrupt_icon_00a"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="corrupt_close"><Type>TextButton</Type><Icon_Texture_Name>back_button.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="hack_shell"><Model_Name>i_hack.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="hack_title01"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="hack_text_00"><Type>TextButton</Type><Group>Hack_Desc</Group></CommandBarComponent>
  <CommandBarComponent Name="hack_cost"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="hack_hack"><Type>TextButton</Type><Icon_Texture_Name>back_button.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="hack_close"><Type>TextButton</Type><Icon_Texture_Name>back_button.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="anim_button"><Type>TextButton</Type><Icon_Texture_Name>i_anim_static.tga</Icon_Texture_Name>
    <Icon_Alternate_Texture_Name>i_anim_00.tga i_anim_01.tga i_anim_02.tga</Icon_Alternate_Texture_Name>
    <Can_Animate>True</Can_Animate><Anim_FPS>5</Anim_FPS><Loop_Anim>False</Loop_Anim></CommandBarComponent>
  <CommandBarComponent Name="anim_upper"><Type>TextButton</Type><Icon_Texture_Name>i_anim_static.tga</Icon_Texture_Name>
    <Icon_Alternate_Texture_Name>i_anim_00.tga i_anim_01.tga i_anim_02.tga</Icon_Alternate_Texture_Name>
    <Can_Animate>True</Can_Animate><Anim_FPS>10</Anim_FPS><Loop_Anim>True</Loop_Anim><Animate_Back>True</Animate_Back>
    <Animate_Upper_Effect>True</Animate_Upper_Effect></CommandBarComponent>
</CommandBarComponents>)";

const char* kFleetXml = R"(<CommandBarComponents>
  <CommandBarComponent Name="q1_fleet"><Type>Button</Type><Icon_Alternate_Texture_Name>i_big.tga i_big.tga</Icon_Alternate_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q2_fleet"><Type>Button</Type><Icon_Alternate_Texture_Name>i_big.tga i_big.tga</Icon_Alternate_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q1_0000"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q1_0001"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q2_0000"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="land_fleet_00"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="land_fleet_01"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="b_play_pause"><Type>TextButton</Type><Icon_Texture_Name>i_big.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_play_pause_t"><Type>TextButton</Type><Icon_Texture_Name>i_big.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="g_planet_fleet"><Type>Button</Type><Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Drag_And_Drop>True</Drag_And_Drop><Size>0.75 5</Size><Offset>3 47</Offset><Default_Offset>36 42</Default_Offset></CommandBarComponent>
  <CommandBarComponent Name="g_planet_land_forces"><Type>Button</Type><Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Drag_And_Drop>True</Drag_And_Drop><Size>0.75 0</Size><Offset>3 5</Offset></CommandBarComponent>
</CommandBarComponents>)";

struct PanelEnv {
    ComponentTable table;
    TextureAtlas atlas;
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    PanelEnv() {
        const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kPanelXml), "p.xml");
        table.parse(doc.root());
        atlas.texture_width = atlas.texture_height = 512;
        for (const char* n : {"e_background.tga", "i_tooltip_rebel_frame.tga", "i_tooltip_empire_frame.tga", "i_tooltip_back.tga",
                              "rebel_hero_ring.tga", "i_black_market_selected.tga", "back_button.tga", "i_button_r_tech_upgrade_5.tga",
                              "i_button_e_tech_upgrade_5.tga", "i_bar_health_small10.tga"})
            atlas.add({n, 0, 0, 40, 40, true});
        atlas.add({"i_button_vader.tga", 0, 0, 64, 64, true});
    }
    // Click the centre of `component`'s hit box through the panel.
    template <class Panel>
    void click(Panel& p, std::string_view name, std::vector<commands::EventData>& cmds, std::vector<HudAction>& acts,
               Button b = Button::Left, double now = 1.0) {
        const int32_t ci = table.find(name);
        const HudHitBox* hb = nullptr;
        for (const HudHitBox& h : p.hit_boxes())
            if (h.component == ci) hb = &h;
        CHECK(hb != nullptr);
        if (!hb) return;
        const Vec2f px = p.canvas().canvas_to_pixel(0.5f * (hb->min.x + hb->max.x), 0.5f * (hb->min.y + hb->max.y));
        CHECK(p.hit_test(static_cast<int32_t>(px.x), static_cast<int32_t>(px.y)) == ci && p.owns(ci));
        RoutedInput ri;
        ri.hover_component = ci;
        ri.hud = {{HudPointerEvent::Kind::Press, b, ci, 0, 0}, {HudPointerEvent::Kind::Release, b, ci, 0, 0},
                  {HudPointerEvent::Kind::Click, b, ci, 0, 0}};
        p.update_input(ri, false, false, now, cmds, &acts);
    }
};

// HUD_GAPS Â§2.2: Encyclopedia_Delay 750 ms (Long_Encyclopedia_Delay 4000 ms with IDC_DELAY_TOOLTIPS), fade in / out at
// Encyclopedia_Fade_Rate 4.0 / s, Encyclopedia_Min_Display_Time 1 s, anchored on the hint bone (not the cursor).
void test_encyclopedia_fade_anchor() {
    PanelEnv env;
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    // Delay.
    {
        EncyclopediaPanel enc;
        enc.init(env.table, env.atlas, env.fonts);
        enc.update(-1, 42, 0.0);
        enc.update(-1, 42, 0.74);
        CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
        enc.update(-1, 42, 0.75);
        CHECK(enc.stage() == EncyclopediaPanel::Stage::Shown && enc.shown_object() == 42 && near(enc.alpha(), 0.0f));
        TooltipConfig slow;
        slow.delay_tooltips = true;
        CHECK(slow.encyclopedia_wait() == 4.0 && TooltipConfig{}.encyclopedia_wait() == 0.75);
        EncyclopediaPanel e2;
        e2.init(env.table, env.atlas, env.fonts);
        e2.set_config(slow);
        e2.update(-1, 42, 0.0);
        e2.update(-1, 42, 3.99);
        CHECK(e2.stage() == EncyclopediaPanel::Stage::Hidden);
        e2.update(-1, 42, 4.0);
        CHECK(e2.stage() == EncyclopediaPanel::Stage::Shown);
        // The countdown restarts when the subject changes or is lost.
        e2.update(-1, 0, 4.1);
        e2.update(-1, 43, 4.2);
        e2.update(-1, 43, 8.1);
        CHECK(e2.shown_object() != 43);
        e2.update(-1, 43, 8.2);
        CHECK(e2.shown_object() == 43 && e2.stage() == EncyclopediaPanel::Stage::Shown);
    }
    // Fade in at 4 / s from the expiry instant, independent of the frame steps.
    {
        EncyclopediaPanel a, b;
        for (EncyclopediaPanel* p : {&a, &b}) {
            p->init(env.table, env.atlas, env.fonts);
            p->update(-1, 7, 0.0);
        }
        a.update(-1, 7, 0.8);
        CHECK(near(a.alpha(), 0.2f));
        a.update(-1, 7, 0.875);
        CHECK(near(a.alpha(), 0.5f));
        b.update(-1, 7, 0.875);  // one step over the expiry
        CHECK(near(b.alpha(), 0.5f));
        a.update(-1, 7, 1.0);
        CHECK(near(a.alpha(), 1.0f));
        a.update(-1, 7, 3.0);
        CHECK(near(a.alpha(), 1.0f));
    }
    // Minimum display time 1 s, then fade out at 4 / s and destroy.
    {
        EncyclopediaPanel a, b;
        for (EncyclopediaPanel* p : {&a, &b}) {
            p->init(env.table, env.atlas, env.fonts);
            p->update(-1, 7, 0.0);
            p->update(-1, 7, 0.9);  // shown at 0.75, alpha 0.6
        }
        CHECK(near(a.alpha(), 0.6f));
        a.update(-1, 0, 1.0);  // lost: still rising until 1.75
        CHECK(near(a.alpha(), 1.0f) && a.shown_object() == 7);
        a.update(-1, 0, 1.75);
        CHECK(near(a.alpha(), 1.0f) && a.stage() == EncyclopediaPanel::Stage::Shown);
        a.update(-1, 0, 1.875);
        CHECK(near(a.alpha(), 0.5f) && a.shown_object() == 7);
        a.update(-1, 0, 2.0);
        CHECK(a.stage() == EncyclopediaPanel::Stage::Hidden && a.alpha() == 0.0f && a.shown_object() == 0);
        b.update(-1, 0, 1.875);  // one step: up to the cap, hold, then down
        CHECK(near(b.alpha(), 0.5f));
        // Lost long after the minimum time: fades out at once.
        EncyclopediaPanel c;
        c.init(env.table, env.atlas, env.fonts);
        c.update(-1, 7, 0.0);
        c.update(-1, 7, 5.0);
        c.update(-1, 0, 5.0625);
        CHECK(near(c.alpha(), 0.75f));
        c.update(-1, 0, 5.25);
        CHECK(c.stage() == EncyclopediaPanel::Stage::Hidden);
    }
    // GameConstants Encyclopedia_Fade_Rate / Encyclopedia_Min_Display_Time.
    {
        const data::XmlDocument doc = data::XmlDocument::parse(
            std::string_view("<GameConstants><Encyclopedia_Delay>750</Encyclopedia_Delay><Long_Encyclopedia_Delay>4000</Long_Encyclopedia_Delay>"
                             "<Encyclopedia_Fade_Rate>2.0</Encyclopedia_Fade_Rate><Encyclopedia_Min_Display_Time>0.5</Encyclopedia_Min_Display_Time>"
                             "</GameConstants>"),
            "gc.xml");
        const TooltipConfig c = tooltip_config_from(doc.root());
        CHECK(near(c.encyclopedia_fade_rate, 2.0f) && std::fabs(c.encyclopedia_min_display - 0.5) < 1e-9 &&
              std::fabs(c.encyclopedia_wait() - 0.75) < 1e-9);
    }
    // Alpha scales every colour; anchor = hint bone (bottom-left corner), not the cursor.
    {
        EncyclopediaEntry e;
        e.header = U"X-wing";
        e.icon = "i_button_vader.tga";
        e.cost = U"350";
        const HudCanvas cv = HudCanvas::make(1024, 768);
        EncyclopediaPanel enc;
        enc.init(env.table, env.atlas, env.fonts);
        enc.update(-1, 9, 0.0);
        enc.update(-1, 9, 0.875);  // alpha 0.5
        HudDrawList dl;
        enc.build(dl, cv, e, {500, 400});
        CHECK(!dl.items(9, HudList::Alpha).empty() && dl.items(9, HudList::Alpha)[0].color.a == 128);  // 255 x 0.5
        enc.update(-1, 9, 2.0);  // alpha 1
        HudLayout layout;
        const auto good = static_cast<uint32_t>(env.table.find("good_hero_frame"));
        const auto evil = static_cast<uint32_t>(env.table.find("evil_hero_frame"));
        ShellBone hb;
        hb.name = "Text_galactic_help";
        hb.x = 65.0f;
        hb.y = 309.0f;
        layout.set_shell_bones(good, {hb});
        ShellBone ab = hb;
        ab.name = "text_adv_help";  // case-insensitive
        ab.x = 300.0f;
        ab.y = 100.0f;
        layout.set_shell_bones(evil, {ab});
        layout.set_shell_active(evil, false);  // advanced map closed
        layout.update(cv, env.table);
        const Vec2f g = layout.shell_offset(good);
        const Vec2f at = enc.hint_anchor(layout, cv);
        CHECK(near(at.x, 65.0f + g.x) && near(at.y, 309.0f + g.y));
        dl.clear();
        const Vec2f sz = enc.build(dl, cv, e, layout);
        CHECK(!dl.items(9, HudList::Alpha).empty());
        if (!dl.items(9, HudList::Alpha).empty()) {
            CHECK(dl.items(9, HudList::Alpha).size() >= 2);
            const HudQuad& frame = dl.items(9, HudList::Alpha)[0];
            const HudQuad& back = dl.items(9, HudList::Alpha)[1];  // over the larger faction frame (only its rim shows)
            CHECK(near(frame.max.x - frame.min.x, 262.0f + 2.0f * kTooltipFrameMargin) && frame.min.x < back.min.x && frame.max.y > back.max.y);
            std::printf("encyclopedia hint anchor (%.1f, %.1f), panel %.0f x %.0f\n", static_cast<double>(at.x),
                        static_cast<double>(at.y), static_cast<double>(sz.x), static_cast<double>(sz.y));
            CHECK(near(back.min.x, at.x) && near(back.min.y, at.y) && near(back.max.x - back.min.x, 262.0f) && back.color.a == 255);  // opaque: nothing of the world shows through
        }
        // The cursor plays no part: the same frame twice is identical.
        HudDrawList dl2;
        enc.build(dl2, cv, e, layout);
        CHECK(dl2.digest() == dl.digest());
        // Advanced map open: its hint bone wins.
        layout.set_shell_active(evil, true);
        layout.update(cv, env.table);
        const Vec2f m = layout.shell_offset(evil);
        const Vec2f at2 = enc.hint_anchor(layout, cv);
        CHECK(near(at2.x, 300.0f + m.x) && near(at2.y, 100.0f + m.y));
        // No hint bone: [port] the galactic bone's base-canvas place.
        const Vec2f at3 = enc.hint_anchor(HudLayout{}, cv);
        CHECK(near(at3.x, -512.0f + 65.0f) && near(at3.y, -384.0f + 309.0f));
    }
}

void test_panels() {
    PanelEnv env;
    // Encyclopedia: hidden until Encyclopedia_Delay (0.75 s), then shown; subject change restarts (fade / anchor details:
    // test_encyclopedia_fade_anchor).
    EncyclopediaPanel enc;
    enc.init(env.table, env.atlas, env.fonts);
    enc.update(5, 0, 1.0);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
    enc.update(5, 0, 1.76);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Shown && enc.shown_component() == 5);
    enc.update(-1, 77, 1.9);
    CHECK(enc.object() == 77 && enc.shown_component() == 5);  // the old pop-up stays / fades out, 77's countdown runs
    enc.update(-1, 77, 2.64);
    CHECK(enc.shown_component() == 5);  // min display until 2.75
    enc.update(-1, 0, 5.1);
    enc.update(-1, 0, 9.0);
    CHECK(enc.stage() == EncyclopediaPanel::Stage::Hidden);
    EncyclopediaEntry e;
    e.header = U"Darth Vader";
    e.icon = "i_button_vader.tga";
    e.cost = U"2500";
    e.rows = {{U"Health", U"3000", {}}, {U"Shields", U"0", {}}, {{}, {}, U"Hero"}};
    e.description = U"Dark Lord of the Sith. Force Crush, Force Push and Saber Throw make him deadly against infantry.";
    const HudCanvas cv = HudCanvas::make(1024, 768);
    HudDrawList dl;
    enc.update(3, 0, 10.0);
    CHECK(enc.build(dl, cv, e, {500, 400}).x == 0.0f && dl.total() == 0);  // hidden
    enc.update(3, 0, 10.8);
    const Vec2f s1 = enc.build(dl, cv, e, {500, 400});
    const size_t items1 = dl.total();
    CHECK(s1.x == 262.0f && s1.y > 3 * 14.0f && dl.items(9, HudList::Alpha).size() >= 3 && dl.items(9, HudList::Text).size() > 10);
    dl.clear();
    enc.update(3, 0, 14.5);
    const Vec2f s2 = enc.build(dl, cv, e, {1000, 10});  // near the top-right corner: kept inside the canvas
    CHECK(s2.y == s1.y && dl.total() == items1);
    for (const HudQuad& q : dl.items(9, HudList::Alpha)) CHECK(q.max.x <= 512.0f + 1e-3f && q.max.y <= 384.0f + 1e-3f);
    dl.clear();
    enc.build(dl, cv, e, {500, 400}, std::nullopt, 0, true);  // zoomed variant on layer 4
    CHECK(!dl.items(4, HudList::Alpha).empty() && dl.items(9, HudList::Alpha).empty());

    // QA U4 (HUD_GAPS Â§2.1): a long "Good against" value wraps in its own column, right of its label, and every glyph stays
    // inside the box; a long name wraps left of the cost; the box grows with the lines. Also pseudo-localised (+40 %).
    {
        const HudCanvas wcv = HudCanvas::make(1024, 768);
        EncyclopediaPanel wenc;
        wenc.init(env.table, env.atlas, env.fonts);
        wenc.update(-1, 9, 0.0);
        wenc.update(-1, 9, 2.0);
        const std::u32string name = U"The Accuser - Captain Piett of the Imperial Fleet and Other Very Long Titles";
        const std::u32string value = U"Corellian Corvette, Corellian Gunship, Interceptor IV Frigate, Carrack Cruiser, Marauder Missile Cruiser";
        const std::u32string word(120, U'W');  // a lone word wider than the box
        EncyclopediaEntry shrt, lng;
        shrt.header = U"X";
        shrt.cost = U"5000";
        EncyclopediaRow r;
        r.left = U"Good against";
        r.right = U"X-wing";
        shrt.rows.push_back(r);
        lng = shrt;
        lng.header = name;
        lng.rows[0].right = value;
        EncyclopediaRow c;
        c.center = word;
        lng.rows.push_back(c);
        const Vec2f at{-200.0f, 200.0f};
        HudDrawList ds, wdl;
        const Vec2f one_line = wenc.build(ds, wcv, shrt, {0.0f, 0.0f}, at);
        const Vec2f big = wenc.build(wdl, wcv, lng, {0.0f, 0.0f}, at);
        CHECK(std::fabs(big.x - one_line.x) < 1e-4f && big.y > one_line.y + 14.0f);
        const float left = at.x, right = at.x + big.x;
        float label_right = -1e9f, value_left = 1e9f;
        const std::span<const HudQuad> text = wdl.items(9, HudList::Text);
        CHECK(!text.empty());
        for (const HudQuad& q : text) {
            CHECK(q.min.x >= left && q.max.x <= right);
            CHECK(q.max.y <= at.y && q.min.y >= at.y - big.y);
        }
        // The label's row: the glyphs left of the box centre sit clear of the value's, so the label stays readable.
        HudDrawList label_only;
        EncyclopediaEntry lb = lng;
        lb.rows[0].right.clear();
        lb.rows.pop_back();
        wenc.build(label_only, wcv, lb, {0.0f, 0.0f}, at);
        HudDrawList with_label;
        EncyclopediaEntry wl = lng;
        wl.rows.pop_back();
        wenc.build(with_label, wcv, wl, {0.0f, 0.0f}, at);
        const size_t header_glyphs = label_only.items(9, HudList::Text).size();
        const std::span<const HudQuad> lt = label_only.items(9, HudList::Text);
        size_t label_n = 0;
        for (char32_t ch : r.left)
            if (ch != U' ') ++label_n;
        CHECK(header_glyphs >= label_n);
        for (size_t i = header_glyphs - label_n; i < header_glyphs; ++i) label_right = std::max(label_right, lt[i].max.x);
        const std::span<const HudQuad> wt = with_label.items(9, HudList::Text);
        for (size_t i = header_glyphs; i < wt.size(); ++i) value_left = std::min(value_left, wt[i].min.x);
        CHECK(wt.size() > header_glyphs && label_right < value_left);
    }

    // t40 (HUD_GAPS sec. 2.2, captures C3_*tooltip*): class + ability icons + Strong / Weak rows. The name and class start at
    // Encyclopedia_Name_Offset, the frames sit flush on the panel's two sides, the box grows by them, the text stays inside.
    {
        const HudCanvas ucv = HudCanvas::make(1024, 768);
        EncyclopediaPanel uenc;
        uenc.init(env.table, env.atlas, env.fonts);
        uenc.update(-1, 9, 0.0);
        uenc.update(-1, 9, 2.0);
        EncyclopediaEntry plain;
        plain.header = U"The Accuser";
        plain.icon = "i_button_vader.tga";
        plain.unit_class = U"Class: Fleet Commander";
        plain.description = U"Imperial fleet commander. Commands enhanced Star Destroyer with improved weaponry.";
        EncyclopediaEntry full = plain;
        full.population = 4;
        full.abilities = {"i_button_vader.tga", "i_button_vader.tga"};
        full.strong_label = U"Strong Against : ";
        full.weak_label = U"Weak Against :";
        full.strong = {"i_button_vader.tga", "i_button_vader.tga", "i_button_vader.tga", "i_button_vader.tga"};  // 3 cells
        full.weak = {"i_button_vader.tga"};
        const Vec2f at{-200.0f, 200.0f};
        HudDrawList pdl, fdl;
        const Vec2f ps = uenc.build(pdl, ucv, plain, {0.0f, 0.0f}, at);
        const Vec2f fs = uenc.build(fdl, ucv, full, {0.0f, 0.0f}, at);
        CHECK(ps.x == 262.0f && fs.x == 262.0f && fs.y >= ps.y + 43.0f && ps.y >= 47.0f);
        CHECK(pdl.items(9, HudList::Alpha).size() + 6 == fdl.items(9, HudList::Alpha).size());  // 2 abilities + 3 + 1 cells
        const std::span<const HudQuad> ft = fdl.items(9, HudList::Text);
        CHECK(!ft.empty());
        for (const HudQuad& q : ft) CHECK(q.min.x >= at.x && q.max.x <= at.x + fs.x && q.max.y <= at.y && q.min.y >= at.y - fs.y);
        float name_left = 1e9f;
        for (const HudQuad& q : pdl.items(9, HudList::Text)) name_left = std::min(name_left, q.min.x);
        CHECK(name_left >= at.x + 5.0f);  // the description starts at the padding
        const std::span<const HudQuad> fa = fdl.items(9, HudList::Alpha);
        float cell_right = -1e9f, cell_bottom = 1e9f;
        for (size_t i = fa.size() - 4; i < fa.size(); ++i) cell_bottom = std::min(cell_bottom, fa[i].min.y);
        for (size_t i = fa.size() - 4; i < fa.size() - 1; ++i) cell_right = std::max(cell_right, fa[i].max.x);
        CHECK(cell_bottom >= at.y - fs.y);
        CHECK(cell_right <= at.x + 129.0f + 1e-3f);  // the strong row's third cell ends with its frame
    }

    // FIXPASS_GALACTIC Â§2.4 / capture C3 (FoC 1.121, Clusters GC): rank ascending, ties by CRC-32 of the upper-case bar type
    // name, unsigned; ranking <= 0 compares equal (gathering order kept). Slot 00 = first entry = rightmost.
    {
        CHECK(hero_tie_key("Boba_Fett") == 0x31FC4F9Au && hero_tie_key("General_Veers") == 0x4B2003E7u &&
              hero_tie_key("Grand_Moff_Tarkin") == 0x8B291F16u && hero_tie_key("Accuser_Star_Destroyer") == 0x9E58A04Bu);
        CHECK(hero_tie_key("boba_fett") == hero_tie_key("BOBA_FETT"));
        struct Hero {
            const char* name;
            int32_t rank;
        };
        auto sorted = [](std::initializer_list<Hero> in) {
            std::vector<HeroEntry> v;
            for (const Hero& h : in) {
                HeroEntry e;
                e.type_name = h.name;
                e.ranking = h.rank;
                v.push_back(std::move(e));
            }
            sort_hero_entries(v);
            std::string out;
            for (const HeroEntry& e : v) out += e.type_name + " ";
            return out;
        };
        // The three bars seen in the captures, gathered in a scrambled order.
        CHECK(sorted({{"Grand_Moff_Tarkin", 3}, {"General_Veers", 3}, {"Emperor_Palpatine", 1}, {"Accuser_Star_Destroyer", 3}, {"Boba_Fett", 3}}) ==
              "Emperor_Palpatine Boba_Fett General_Veers Grand_Moff_Tarkin Accuser_Star_Destroyer ");
        CHECK(sorted({{"Droid_R2D2", 5}, {"Sundered_Heart", 3}, {"Millennium_Falcon", 2}, {"Obi_Wan_Kenobi", 2}, {"Mon_Mothma", 1}, {"Rogue_Squadron_Space", 0}}) ==
              "Mon_Mothma Obi_Wan_Kenobi Millennium_Falcon Sundered_Heart Droid_R2D2 Rogue_Squadron_Space ");
        CHECK(sorted({{"IG-2000", 3}, {"Silri", 3}, {"HoundsTooth", 3}, {"Urai_Fen", 2}, {"The_Peacebringer", 1}}) ==
              "The_Peacebringer Urai_Fen HoundsTooth Silri IG-2000 ");
        std::vector<HeroEntry> z(3);
        z[0].object = 1;
        z[1].object = 2;
        z[1].ranking = 1;
        z[2].object = 3;
        sort_hero_entries(z);
        CHECK(z[0].object == 1 && z[1].object == 2 && z[2].object == 3);  // ranking 0 -> unchanged
    }
    // Hero roster: two heroes on the good frame; click -> HeroSelect; bar hidden while respawning.
    HeroPanel heroes;
    CHECK(heroes.init(env.table, env.atlas, env.fonts));
    heroes.resize(1024, 768);
    CHECK(heroes.shells().size() == 2 && heroes.shells()[0] == env.table.find("good_hero_frame"));
    HeroEntry hs[2];
    hs[0].object = 4001;
    hs[0].icon = "i_button_vader.tga";
    hs[0].health = 0.5f;
    hs[0].planet = 7001;
    hs[1].object = 4002;
    hs[1].icon = "i_button_vader.tga";
    hs[1].health = 0.0f;
    hs[1].respawn = 0.3f;
    hs[1].selected = true;
    hs[1].planet = 7002;
    hs[1].container = 5002;
    HeroPanelFrame hf;
    hf.open = true;
    hf.good = true;
    hf.heroes = hs;
    heroes.build(hf);
    CHECK(heroes.open() && heroes.active_shell() == env.table.find("good_hero_frame") && heroes.hit_boxes().size() == 2 &&
          heroes.slots().size() == 4);
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    // FIXPASS_GALACTIC Â§2.2 galactic: left -> HeroFocusPlanet {hero, {planet}}; double -> + HeroZoomPlanet; right: nothing.
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.0);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroFocusPlanet && acts[0].unit == 4002 &&
          acts[0].ids == std::vector<uint32_t>{7002} && cmds.empty());
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.3);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HeroFocusPlanet &&
          acts[1].kind == HudAction::Kind::HeroZoomPlanet && acts[1].ids == std::vector<uint32_t>{7002});
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 1.5);  // third click: a new single
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroFocusPlanet);
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 3.0);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 3.7);  // too slow: two singles
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HeroFocusPlanet && acts[1].kind == HudAction::Kind::HeroFocusPlanet &&
          acts[1].unit == 4001);
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Right, 5.0);
    CHECK(acts.empty());
    hs[0].planet = commands::kNoObject;  // hyperspace
    hs[1].object = 0;                    // building / respawning
    heroes.build(hf);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 7.0);
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 9.0);
    CHECK(acts.empty());
    // Â§2.3 tactical: left -> HeroSelect; double -> + HeroSelectGroup {container} / {} (by type).
    hs[0].planet = 7001;
    hs[1].object = 4002;
    hf.tactical = true;
    heroes.build(hf);
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 11.0);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::HeroSelect && acts[0].unit == 4002 &&
          acts[0].ids == std::vector<uint32_t>{4002});
    acts.clear();
    env.click(heroes, "hero_slot_good01", cmds, acts, Button::Left, 11.2);
    CHECK(acts.size() == 2 && acts[1].kind == HudAction::Kind::HeroSelectGroup && acts[1].unit == 4002 &&
          acts[1].ids == std::vector<uint32_t>{5002});
    acts.clear();
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 13.0);
    env.click(heroes, "hero_slot_good00", cmds, acts, Button::Left, 13.2);
    CHECK(acts.size() == 3 && acts[2].kind == HudAction::Kind::HeroSelectGroup && acts[2].unit == 4001 && acts[2].ids.empty());
    acts.clear();
    // Hover: the good slot's hero for the encyclopedia.
    {
        RoutedInput ri;
        ri.hover_component = env.table.find("hero_slot_good00");
        heroes.update_input(ri, false, false, 14.0, cmds, &acts);
        CHECK(heroes.hover_component() == ri.hover_component && heroes.hover_hero() == 4001 && acts.empty());
    }
    hf.tactical = false;
    hf.good = false;
    heroes.build(hf);
    CHECK(heroes.active_shell() == env.table.find("evil_hero_frame") && heroes.hit_boxes().size() == 1);  // only evil00 exists
    env.click(heroes, "hero_slot_evil00", cmds, acts, Button::Left, 20.0);
    env.click(heroes, "hero_slot_evil00", cmds, acts, Button::Left, 20.1);
    CHECK(acts.empty() && heroes.hover_hero() == 0);  // Â§2.1: enemy slots have no callbacks
    hf.open = false;
    heroes.build(hf);
    CHECK(!heroes.open() && heroes.hit_boxes().empty() && heroes.draw_list().total() == 0 && heroes.hit_test(512, 384) == -1);

    // Black market: item click -> BlackMarketBuy{item, planet}; disabled item -> nothing; close / right click -> PanelClose.
    BlackMarketPanel bm;
    CHECK(bm.init(env.table, env.atlas, env.fonts));
    bm.resize(1920, 1080);
    const BlackMarketItem items[2] = {{301, "i_button_vader.tga", U"Ion cannon upgrade", U"Hits harder.", 1200, true, 1},
                                      {302, "i_button_vader.tga", U"Shield booster", U"", 900, false, -1}};
    BlackMarketFrame bf;
    bf.open = true;
    bf.planet = 555;
    bf.title = U"Black Market";
    bf.items = items;
    bm.build(bf);
    acts.clear();
    env.click(bm, "market_01", cmds, acts);  // disabled: swallowed
    env.click(bm, "market_00", cmds, acts);
    CHECK(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::BlackMarketBuy && acts[0].unit == 301 &&
          acts[0].value == 0 && acts[0].ids == std::vector<uint32_t>{555});
    acts.clear();
    // hovering item 0 shows its description / cost on market_title02 / 03 at the next build
    bm.build(bf);
    bool desc = false;
    for (const PanelSlot& s : bm.slots())
        if (s.component == env.table.find("market_title02")) desc = s.text == U"Hits harder.";
    CHECK(desc);
    env.click(bm, "market_close", cmds, acts);
    env.click(bm, "market_00", cmds, acts, Button::Right);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::PanelClose && acts[0].name == "market_frame" &&
          acts[1].kind == HudAction::Kind::PanelClose && cmds.empty());
    // Slice dialog (market_frame stand-in): 7 rows, 5 visible; row click -> SliceSelect{type, {planet, slicer}, row};
    // unaffordable row swallowed; the wheel over the panel scrolls; hovered row shows "Chance n%"; close -> PanelClose.
    {
        SlicePanel sp;
        CHECK(sp.init(env.table, env.atlas, env.fonts));
        CHECK(sp.shells().size() == 1 && sp.shells()[0] == env.table.find("market_frame"));
        std::vector<SliceRow> rows;
        for (uint32_t k = 0; k < 7; ++k)
            rows.push_back({500 + k, "i_button_vader.tga", U"Type", U"", 1000 + static_cast<int32_t>(k), 1.0f, k != 1});
        SliceFrame sf2;
        sf2.open = true;
        sf2.planet = 557;
        sf2.slicer = 9001;
        sf2.title = U"Slice";
        sf2.rows = rows;
        sp.build(sf2);
        CHECK(sp.open() && sp.active_shell() == env.table.find("market_frame") && sp.first_row() == 0);
        acts.clear();
        env.click(sp, "market_01", cmds, acts);  // unaffordable: swallowed
        env.click(sp, "market_00", cmds, acts);
        CHECK(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::SliceSelect && acts[0].unit == 500 &&
              acts[0].value == 0 && acts[0].ids == (std::vector<uint32_t>{557, 9001}));
        sp.build(sf2);  // market_00 hovered: its chance on market_title02
        bool chance = false;
        for (const PanelSlot& s : sp.slots())
            if (s.component == env.table.find("market_title02")) chance = s.text == U"Chance 100%";
        CHECK(chance);
        RoutedInput wheel;
        wheel.wheel_target = PointerTarget::Hud;
        wheel.hover_component = env.table.find("market_00");
        sp.update_input(wheel, false, false, 2.0, cmds, &acts, -5.0f);  // towards the user: down, clamped to 7 - 5
        sp.build(sf2);
        CHECK(sp.first_row() == 2);
        acts.clear();
        env.click(sp, "market_00", cmds, acts);
        CHECK(acts.size() == 1 && acts[0].unit == 502 && acts[0].value == 2);
        wheel.hover_component = -1;  // not over the panel: ignored
        sp.update_input(wheel, false, false, 2.1, cmds, &acts, 1.0f);
        CHECK(sp.first_row() == 2);
        acts.clear();
        env.click(sp, "market_close", cmds, acts);
        CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::PanelClose && acts[0].name == "market_frame" && cmds.empty());
        sf2.open = false;  // reopening starts at the first row
        sp.build(sf2);
        sf2.open = true;
        sp.build(sf2);
        CHECK(sp.first_row() == 0);
    }
    // Sabotage and corruption.
    SabotagePanel sab;
    CHECK(sab.init(env.table, env.atlas, env.fonts));
    const SabotageTarget tg[2] = {{8001, "i_button_vader.tga", U"Barracks", 300, true}, {8002, "i_button_vader.tga", U"Turbolaser", 500, true}};
    SabotageFrame sf;
    sf.open = true;
    sf.planet = 556;
    sf.targets = tg;
    sab.build(sf);
    acts.clear();
    env.click(sab, "bm_0001", cmds, acts);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::SabotageTarget && acts[0].unit == 8002 && acts[0].ids == std::vector<uint32_t>{556});
    CorruptionPanel cor;
    CHECK(cor.init(env.table, env.atlas, env.fonts));
    CorruptionOption opts[2];
    opts[0].corruption_type = 1;
    opts[0].icon = "i_button_vader.tga";
    opts[0].title = U"Piracy";
    opts[0].requirements = {"i_button_vader.tga"};
    opts[1].corruption_type = 6;
    opts[1].icon = "i_button_vader.tga";
    opts[1].enabled = false;
    CorruptionFrame cf;
    cf.open = true;
    cf.planet = 557;
    cf.options = opts;
    cor.build(cf);
    acts.clear();
    env.click(cor, "corrupt_button_icon_a", cmds, acts);
    env.click(cor, "corrupt_button_icon_b", cmds, acts);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::CorruptionChoice && acts[0].unit == 557 && acts[0].value == 1);
    // A component of another panel is not owned.
    CHECK(!cor.owns(env.table.find("market_00")) && cmds.empty());
}

// ---- P4 completion 2: remaining screens ---------------------------------------------------------------------------------
void test_info_screens() {
    Env env;
    InfoScreens is;
    is.init(env.gui);
    std::vector<ScreenAction> acts;
    // Skirmish buy units.
    BuyUnitsState bs;
    bs.credits = 1000;
    bs.units = {{"Rebel_Infantry_Squad", U"Infantry", 200, 0, "i_unit.tga", true}, {"T2B_Company", U"T2-B", 1200, 2, "i_tank.tga", true}};
    CHECK(is.show_buy_units(bs));
    GuiDialog* d = env.gui.find(dlg::kBuyUnits);
    CHECK(d && d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX")->items.size() == 2 &&
          d->get("IDC_SKIRMISH_BUY_FORCES_LISTBOX")->items[1] == U"T2-B  1200  x2" && d->get("IDC_BUY_BUTTON")->enabled &&
          !d->get("IDC_SELL_BUTTON")->enabled && d->get("IDC_SELL_ALL_BUTTON")->enabled &&
          d->get("IDC_SKIRMISH_BUY_MENU_UNIT_IMAGE")->image == "i_unit.tga" &&
          d->get("IDC_SKIRMISH_BUY_UNITS_CREDITS_TEXT")->text == U"Credits: 1000");
    is.update(env.click(dlg::kBuyUnits, "IDC_BUY_BUTTON"), acts);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::BuyUnit && acts[0].map == "Rebel_Infantry_Squad");
    d->set_selected("IDC_SKIRMISH_BUY_FORCES_LISTBOX", 1);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kBuyUnits, "IDC_SKIRMISH_BUY_FORCES_LISTBOX", 1)}, acts);
    CHECK(!d->get("IDC_BUY_BUTTON")->enabled && d->get("IDC_SELL_BUTTON")->enabled);  // 1200 > 1000 credits
    is.update(env.click(dlg::kBuyUnits, "IDC_SELL_BUTTON"), acts);
    is.update(env.click(dlg::kBuyUnits, "IDC_NEXT_BUTTON"), acts);
    CHECK(acts.size() == 3 && acts[1].kind == ScreenAction::Kind::SellUnit && acts[1].map == "T2B_Company" &&
          acts[2].kind == ScreenAction::Kind::BuyUnitsDone && env.gui.find(dlg::kBuyUnits) == nullptr);
    // Story dialog.
    acts.clear();
    StoryDialogInfo st;
    st.tag = "intro";
    st.title = U"A new hope";
    st.lines = {U"Line 1", U"Line 2"};
    st.cancel = true;
    CHECK(is.show_story(st));
    d = env.gui.find(dlg::kStory);
    CHECK(d && d->get("IDC_STORY_TEXT")->items.size() == 2 && d->get("IDC_STORY_DIALOG_CANCEL")->visible && !d->get("IDC_IMAGE")->visible);
    is.update(env.click(dlg::kStory, "IDC_STORY_DIALOG_ACCEPT"), acts);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::StoryAccepted && acts[0].tag == "intro");
    // Story dialog script timing: coloured lines appear at their WAIT times, SFX / speech cues fire once each.
    {
        StoryDialogInfo sc;
        sc.tag = "timed";
        sc.lines = {U"Title", U"", U"Body", U"Objective"};
        sc.line_colors = {{128, 55, 136, 255}, {0, 0, 0, 0}, {255, 255, 255, 176}, {109, 247, 255, 255}};
        sc.line_at = {0.0, 0.0, 0.5, 1.5};
        sc.cues = {{0.5, StoryDialogCue::Kind::Sfx, "Button_Press"}, {0.5, StoryDialogCue::Kind::Speech, "MON_01"},
                   {1.5, StoryDialogCue::Kind::Sfx, "Button_Press"}};
        CHECK(is.show_story(sc));
        const GuiControl* tx = env.gui.find(dlg::kStory) ? env.gui.find(dlg::kStory)->get("IDC_STORY_TEXT") : nullptr;
        CHECK(tx && tx->items.size() == 2 && tx->item_text_colors.size() == 2 && tx->item_text_colors[0].r == 128);
        std::vector<StoryDialogCue> due;
        CHECK(is.update_story(10.0, &due) && due.empty() && is.story_lines_shown() == 2);  // the clock starts here
        CHECK(is.update_story(10.6, &due) && is.story_lines_shown() == 3 && due.size() == 2 && due[1].kind == StoryDialogCue::Kind::Speech &&
              due[1].event == "MON_01");
        CHECK(!is.update_story(11.6, &due) && is.story_lines_shown() == 4 && due.size() == 3);
        CHECK(!is.update_story(12.0, &due) && due.size() == 3);  // cues fire once
        CHECK(tx && tx->items.size() == 4 && tx->item_text_colors.size() == 4 && tx->item_text_colors[3].g == 247);
        acts.clear();
        is.update(env.click(dlg::kStory, "IDC_STORY_DIALOG_ACCEPT"), acts);
        CHECK(acts.size() == 1 && acts[0].tag == "timed" && !is.update_story(13.0, &due));
    }
    // GALACTIC_DIALOGS Â§9 IDD_PLANET_INFO (GameScreens): name line in the owner colour, overlay, wrapped lists (tactical in
    // 50/200/255), icon-name rewrites, map icon off when missing; single instance; Close / Esc -> PlanetInfoClosed; host close.
    {
        CHECK(planet_info_weather_icon("i_pa_weather_rain.tga") == "load_weather_rain.tga" && planet_info_weather_icon("").empty());
        CHECK(planet_info_map_icon("_Space_Planet_Kuat.TED") == "_Space_Planet_Kuat.dds" && planet_info_map_icon("Kuat") == "Kuat.dds" &&
              planet_info_map_icon("").empty());
        GameScreens gs2;
        gs2.init(env.gui);
        PlanetInfoData pi;
        pi.planet = 88;
        pi.name = U"Kuat";
        pi.name_color = Rgba8{200, 30, 30, 255};
        pi.planet_icon = "planet_render_kuat";
        std::u32string longline;
        for (int k = 0; k < 40; ++k) longline += U"shipyards ";
        pi.tactical = {longline + U"\nSecond paragraph."};
        pi.advantages = {U"+25% space unit production"};
        pi.natives = U"Human";
        pi.terrain = U"Urban";
        pi.weather = U"Clear";
        pi.weather_icon = planet_info_weather_icon("i_pa_weather_clear.tga");
        pi.native_icon = "load_urban.tga";
        pi.map_icon = "";  // the .dds is missing: control off
        CHECK(!gs2.planet_info_open() && gs2.show_planet_info(pi) && gs2.planet_info_open());
        const GuiDialog* pd2 = env.gui.find(dlg::kPlanetInfo);
        CHECK(pd2 != nullptr);
        if (pd2) {
            const GuiControl* nm = pd2->get("IDC_STATIC_PLANET_NAME");
            CHECK(nm && nm->text.size() > 5 && nm->text.substr(nm->text.size() - 5) == U" Kuat" && nm->tint.r == 200 && nm->tint.g == 30);
            const GuiControl* tac = pd2->get("IDC_LIST_TACTICAL");
            CHECK(tac && tac->items.size() >= 3 && tac->top == 0 && tac->item_text_colors.size() == tac->items.size() &&
                  tac->item_text_colors[0].g == 200 && tac->item_text_colors[0].b == 255 && tac->items.back() == U"Second paragraph.");
            const GuiControl* adv = pd2->get("IDC_LIST_ADVANTAGE");
            CHECK(adv && adv->items.size() == 1 && adv->item_text_colors.size() == 1 && adv->item_text_colors[0].a == 0);
            CHECK(pd2->get("IDC_LIST_REFERENCE")->items.empty());
            CHECK(pd2->get("IDC_PLANET_ICON")->image == "planet_render_kuat" && pd2->get("IDC_PLANET_OVERLAY")->image == "load_overlay.tga" &&
                  pd2->get("IDC_PLANET_OVERLAY")->visible);
            CHECK(pd2->get("IDC_WEATHER_ICON")->image == "load_weather_clear.tga" && pd2->get("IDC_NATIVE_ICON")->image == "load_urban.tga" &&
                  !pd2->get("IDC_MAP_ICON")->visible);
            CHECK(pd2->get("IDC_STATIC_NATIVE")->text == U"Human" && pd2->get("IDC_STATIC_TERRAIN")->text == U"Urban" &&
                  pd2->get("IDC_STATIC_WEATHER")->text == U"Clear");
        }
        PlanetInfoData other = pi;
        other.planet = 99;
        other.natives = U"Wookiee";
        CHECK(!gs2.show_planet_info(other));  // single instance: ignored, first planet kept
        CHECK(env.gui.find(dlg::kPlanetInfo)->get("IDC_STATIC_NATIVE")->text == U"Human");
        std::vector<ScreenAction> pa;
        std::vector<commands::EventData> pc;
        gs2.update(env.click(dlg::kPlanetInfo, "IDC_CLOSE_BUTTON"), pa, pc);
        CHECK(pa.size() == 1 && pa[0].kind == ScreenAction::Kind::PlanetInfoClosed && pa[0].object == 88 && !gs2.planet_info_open());
        other.map_icon = planet_info_map_icon("Kuat.ted");
        CHECK(gs2.show_planet_info(other) && env.gui.find(dlg::kPlanetInfo)->get("IDC_MAP_ICON")->image == "Kuat.dds" &&
              env.gui.find(dlg::kPlanetInfo)->get("IDC_MAP_ICON")->visible);
        pa.clear();
        gs2.update(env.frame({5, 5}, false, {vk::kEscape}), pa, pc);
        CHECK(pa.size() == 1 && pa[0].kind == ScreenAction::Kind::PlanetInfoClosed && pa[0].object == 99 && !gs2.planet_info_open());
        CHECK(gs2.show_planet_info(pi));
        gs2.close_planet_info();  // host: a battle load screen starts
        CHECK(!gs2.planet_info_open() && pc.empty());
    }
    // Planet dialog (IDD_PLANET_DIALOG: dead in the original, kept).
    PlanetDialogInfo pd;
    pd.planet = 77;
    pd.name = U"Kuat";
    pd.credits = 120;
    pd.political = {"i_pol.tga"};
    pd.producing = U"Star Destroyer";
    pd.progress = 0.25f;
    CHECK(is.show_planet(pd));
    d = env.gui.find(dlg::kPlanetDialog);
    CHECK(d && d->get("IDC_TEXT_PLANET_CREDITS")->text == U"+120" && d->get("IDC_POLITICAL_ICON_0")->visible &&
          !d->get("IDC_POLITICAL_ICON_1")->visible && d->get("IDC_PRODUCTION_MENU_PROGRESS")->value == 25);
    // REMAINING_GAPS Â§6.3 gates: hidden starbase / ground-base info blanks those icon rows.
    {
        PlanetDialogInfo hidden = pd;
        hidden.starbase = {"i_sb.tga", "i_sb.tga"};
        hidden.ground_base = {"i_gb.tga"};
        CHECK(is.show_planet(hidden));
        const GuiDialog* hd = env.gui.find(dlg::kPlanetDialog);
        const GuiControl* sb = hd ? hd->get("IDC_STARBASE_ICON_0") : nullptr;
        const GuiControl* gb = hd ? hd->get("IDC_GROUND_BASE_ICON_0") : nullptr;
        CHECK(sb && gb && sb->visible && gb->visible);
        hidden.starbase_info = false;
        hidden.groundbase_info = false;
        CHECK(is.show_planet(hidden));
        CHECK(sb && gb && !sb->visible && !gb->visible && hd->get("IDC_POLITICAL_ICON_0")->visible);
        hidden.groundbase_info = true;
        CHECK(is.show_planet(hidden));
        CHECK(sb && gb && !sb->visible && gb->visible);
        CHECK(is.show_planet(pd));
    }
    is.update(env.click(dlg::kPlanetDialog, "IDC_PLANET_PRODUCTION_BUTTON"), acts);
    is.update(env.frame({5, 5}, false, {vk::kEscape}), acts);
    CHECK(acts.size() == 3 && acts[1].kind == ScreenAction::Kind::PlanetProduction && acts[1].object == 77 &&
          acts[2].kind == ScreenAction::Kind::PlanetDialogClosed);
    // Holocron: data chart -> tech tree -> space variant -> summary (scroll) -> close.
    acts.clear();
    DataChartInfo dc;
    dc.economy = {{U"Empire", {200, 30, 30, 255}, {100, 300, 900, 1200}}, {U"Rebels", {40, 90, 220, 255}, {150, 200, 250, 400}}};
    dc.military = {{U"Empire", {200, 30, 30, 255}, {5, 9}}};
    dc.first_cycle = 3;
    is.set_data_chart(dc);
    TechTreeInfo tt;
    tt.faction = TechFaction::Empire;
    tt.item = [](std::string_view type, TechTreeItem& out) {
        if (type == "Dark_Trooper_PhaseI") return false;
        out.icon = "i_tech.tga";
        out.state = type == "E_Ground_Barracks" ? TechState::Researched : TechState::Locked;
        return true;
    };
    is.set_tech_tree(tt);
    SummaryInfo sm;
    for (int i = 0; i < 9; ++i) sm.production.push_back({"i_planet.tga", U"P" + std::u32string(1, static_cast<char32_t>(U'0' + static_cast<char32_t>(i))), "", "", U"", U"", {"i_hero.tga"}, U"5"});
    is.set_summary(sm);
    CHECK(is.show_info(InfoPage::DataChart) && is.info_dialog() == dlg::kDataChart);
    d = env.gui.find(dlg::kDataChart);
    CHECK(d->get("IDC_Y_MAX")->text == U"1200" && d->get("IDC_STATIC_END")->text == U"6" && d->get("IDC_DATA_CHART_RADIO")->checked);
    HudDrawList dl;
    env.gui.build(dl, env.now);
    const size_t dots = is.build_overlay(dl);
    CHECK(dots > 100);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Checked, dlg::kDataChart, "IDC_RADIO_MILITARY", 1)}, acts);
    CHECK(is.chart_mode() == 1 && d->get("IDC_Y_MAX")->text == U"9");
    is.update(env.click(dlg::kDataChart, "IDC_TECH_TREE_RADIO"), acts);
    CHECK(is.info_page() == InfoPage::TechTree && is.info_dialog() == "IDD_TECH_TREE_EMPIRE" && !acts.empty() &&
          acts.back().kind == ScreenAction::Kind::InfoPageChanged && acts.back().button == static_cast<int32_t>(InfoPage::TechTree));
    d = env.gui.find("IDD_TECH_TREE_EMPIRE");
    CHECK(d && env.gui.find(dlg::kDataChart) == nullptr && d->get("IDC_TECHTREE_E_Ground_Barracks")->tint == (Rgba8{150, 255, 150, 255}) &&
          d->get("IDC_TECHTREE_Imperial_Stormtrooper_Squad")->tint == (Rgba8{70, 70, 70, 255}) &&
          !d->get("IDC_TECHTREE_Dark_Trooper_PhaseI")->visible && d->get("IDC_TECH_TREE_LAND")->checked);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Checked, "IDD_TECH_TREE_EMPIRE", "IDC_TECH_TREE_SPACE", 1)}, acts);
    CHECK(is.info_dialog() == "IDD_TECH_TREE_EMPIRE_SPACE" && env.gui.find("IDD_TECH_TREE_EMPIRE_SPACE")->get("IDC_TECHTREE_Star_Destroyer")->visible);
    CHECK(is.show_info(InfoPage::SummaryProduction));
    d = env.gui.find(dlg::kSummaryProduction);
    CHECK(d && d->get("IDC_PLANET_NAME0")->text == U"P0" && d->get("IDC_HERO0_ICON0")->visible && d->get("IDC_SUMMARY_SCREEN_RADIO")->checked);
    is.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, dlg::kSummaryProduction, "IDC_SCROLLBAR_SLIDER", 100)}, acts);
    CHECK(is.summary_offset() == 3 && d->get("IDC_PLANET_NAME0")->text == U"P3");
    acts.clear();
    is.update(env.click(dlg::kSummaryProduction, "IDC_BUTTON_POP_CAP"), acts);
    is.update(env.click(dlg::kSummaryProduction, "IDC_BUTTON_EXIT"), acts);
    CHECK(acts.size() == 2 && acts[0].kind == ScreenAction::Kind::SummarySort && acts[0].tag == "IDC_BUTTON_POP_CAP" &&
          acts[1].kind == ScreenAction::Kind::InfoClosed && is.info_dialog().empty() && env.gui.open_count() == 0);
    CHECK(tech_tree_dialog(TechFaction::BlackMarket, true) == "IDD_TECH_TREE_BLACKMARKET_SPACE");
}

// ---- mouse cursor ---------------------------------------------------------------------------------------------------
void test_cursor() {
    const char* xml = R"(<MousePointers>
      <MousePointer Name="POINTER_NORMAL"><Base_Texture> DEFAULTPOINTER_00.TGA </Base_Texture><Hot_X>0</Hot_X><Hot_Y>0</Hot_Y><Anim_Frame_Delay>5</Anim_Frame_Delay></MousePointer>
      <MousePointer Name="POINTER_ATTACK"><Base_Texture>ATTACKPOINTER_00.TGA</Base_Texture><Hot_X>16</Hot_X><Hot_Y>16</Hot_Y><Anim_Frame_Delay>3</Anim_Frame_Delay></MousePointer>
      <MousePointer Name="POINTER_MOVIE_MODE"><Base_Texture>MOVIEPOINTER.TGA</Base_Texture><Hot_X>0</Hot_X><Hot_Y>0</Hot_Y></MousePointer>
    </MousePointers>)";
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(xml), "MousePointers.xml");
    CursorTable t;
    CHECK(t.parse(doc.root()) == 3);
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 256;
    for (const char* n : {"DEFAULTPOINTER_00.TGA", "ATTACKPOINTER_00.TGA", "ATTACKPOINTER_01.TGA", "ATTACKPOINTER_02.TGA"})
        atlas.add({n, 0, 0, 32, 32, true});
    CHECK(t.resolve_frames([&](std::string_view n) { return atlas.find(n) != nullptr; }) == 5);
    CHECK(t.find(Pointer::Attack)->frames.size() == 3 && t.find(Pointer::Attack)->frames[2] == "ATTACKPOINTER_02.TGA" &&
          t.find(Pointer::MovieMode)->frames.size() == 1);
    Pointer p;
    CHECK(pointer_from_name("pointer_repair_hardpoint", p) && p == Pointer::RepairHardpoint && kPointerCount == 53);
    // Context -> pointer.
    CursorContext c;
    CHECK(choose_pointer(c) == Pointer::Normal);
    c.hover = CursorContext::Hover::Enemy;
    CHECK(choose_pointer(c) == Pointer::Normal);  // nothing selected
    c.selection = true;
    CHECK(choose_pointer(c) == Pointer::Attack);
    c.in_range = false;
    CHECK(choose_pointer(c) == Pointer::AttackOutOfRange);
    c.hover = CursorContext::Hover::Ground;
    c.passable = false;
    CHECK(choose_pointer(c) == Pointer::CantMove);
    c.targeting = CursorContext::Targeting::AbilityEnemy;
    c.target_valid = false;
    CHECK(choose_pointer(c) == Pointer::AbilityEnemyInvalid);
    c.over_ui = true;
    c.over_draggable = true;
    CHECK(choose_pointer(c) == Pointer::CanGrab);
    c.fleet_drag = true;
    c.drop_on_planet = true;
    CHECK(choose_pointer(c) == Pointer::Drop);
    c.busy = true;
    CHECK(choose_pointer(c) == Pointer::Wait);
    // Animation: 3 frames, 3 ticks (0.1 s) each; restarts on change.
    MouseCursor mc;
    mc.init(t, &atlas, {});
    mc.update(Pointer::Attack, 10.0);
    CHECK(mc.frame(10.05) == 0 && mc.frame(10.15) == 1 && mc.frame(10.25) == 2 && mc.frame(10.35) == 0);
    CHECK(mc.texture(10.15) == "ATTACKPOINTER_01.TGA");
    const HudCanvas cv = HudCanvas::make(1024, 768);
    HudDrawList dl;
    CHECK(mc.build(dl, cv, {512, 384}, 10.0));
    const auto q = dl.items(kHudLayers - 1, HudList::Alpha);
    CHECK(q.size() == 1 && std::fabs(q[0].min.x + 16.0f) < 1e-3f && std::fabs(q[0].max.y - 16.0f) < 1e-3f &&
          std::fabs(q[0].max.x - 16.0f) < 1e-3f);  // hot spot 16,16 at the cursor
    mc.update(Pointer::CantGrab, 11.0);  // no entry -> falls back to POINTER_NORMAL
    dl.clear();
    CHECK(mc.build(dl, cv, {0, 0}, 11.0) && mc.texture(11.0) == "DEFAULTPOINTER_00.TGA");
}

// ---- Can_Animate, hack panel, fleet drag and drop -------------------------------------------------------------------
void test_anim_hack_drag() {
    PanelEnv env;
    for (const char* n : {"i_anim_static.tga", "i_anim_00.tga", "i_anim_01.tga", "i_anim_02.tga"})
        env.atlas.add({n, static_cast<uint32_t>(env.atlas.size()) * 8u, 0, 8, 8, true});
    const CommandBarComponent& ab = *env.table.get("anim_button");
    const CommandBarComponent& au = *env.table.get("anim_upper");
    CHECK(ab.can_animate && ab.anim_fps == 5.0f);
    // anim_button: 5 fps, 3 frames, one pass; anim_upper: 10 fps ping-pong 0 1 2 1 0 1 ..., looping.
    CHECK(anim_frame(ab, 1.0, 1.0, 3, false) == 0 && anim_frame(ab, 1.0, 1.21, 3, false) == 1 && anim_frame(ab, 1.0, 1.45, 3, false) == 2 &&
          anim_frame(ab, 1.0, 1.61, 3, false) == -1 && anim_frame(ab, 1.0, 1.61, 3, true) == 0);
    CHECK(anim_frame(au, 0.0, 0.25, 3, true) == 2 && anim_frame(au, 0.0, 0.35, 3, true) == 1 && anim_frame(au, 0.0, 0.45, 3, true) == 0 &&
          anim_frame(au, 0.0, 0.55, 3, true) == 1);
    HudButtons b;
    b.bind(env.table);
    const auto ci = static_cast<uint32_t>(env.table.find("anim_button"));
    const HudCanvas cv = HudCanvas::make(1024, 768);
    auto icon_u = [&](double now, HudList list) {
        HudDrawList dl;
        b.draw(dl, env.atlas, nullptr, cv, ci, {0, 0}, {}, 1, now);
        const auto q = dl.items(0, list);
        return q.empty() ? -1.0f : q.back().uv0.x * 512.0f;
    };
    const float idle = icon_u(0.0, HudList::Alpha);
    b.play_anim(ci, 5.0);
    const float f1 = icon_u(5.25, HudList::Alpha);
    CHECK(idle != f1 && f1 == static_cast<float>(env.atlas.find("i_anim_01.tga")->x));
    CHECK(icon_u(6.0, HudList::Alpha) == idle);  // one pass done
    const auto cu = static_cast<uint32_t>(env.table.find("anim_upper"));
    b.play_anim(cu, 0.0, true);
    HudDrawList dl;
    b.draw(dl, env.atlas, nullptr, cv, cu, {0, 0}, {}, 1, 0.05);
    // static icon, then the frame over it (later in the same PrimAlpha list)
    CHECK(dl.items(0, HudList::Alpha).size() == 2 && dl.items(0, HudList::Alpha)[1].uv0.x * 512.0f != dl.items(0, HudList::Alpha)[0].uv0.x * 512.0f);
    b.stop_anim(cu);
    dl.clear();
    b.draw(dl, env.atlas, nullptr, cv, cu, {0, 0}, {}, 1, 0.05);
    CHECK(dl.items(0, HudList::Alpha).size() == 1);

    // Hack panel.
    HackPanel hp;
    CHECK(hp.init(env.table, env.atlas, env.fonts));
    HackFrame hf;
    hf.open = true;
    hf.target = 9001;
    hf.planet = 600;
    hf.title = U"Hack";
    hf.lines = {U"Disable the shield generator"};
    hf.cost = U"500";
    hp.build(hf);
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    env.click(hp, "hack_hack", cmds, acts);
    env.click(hp, "hack_close", cmds, acts);
    CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::HackConfirm && acts[0].unit == 9001 && acts[0].ids == std::vector<uint32_t>{600} &&
          acts[1].kind == HudAction::Kind::PanelClose && acts[1].name == "hack_shell" && cmds.empty());

    // Fleet drag and drop on the galactic HUD.
    ComponentTable ft;
    const data::XmlDocument fd = data::XmlDocument::parse(std::string_view(kFleetXml), "f.xml");
    ft.parse(fd.root());
    TextureAtlas fa;
    fa.texture_width = fa.texture_height = 64;
    fa.add({"i_big.tga", 0, 0, 30, 30, true});
    fa.add({"i_frame.tga", 0, 0, 40, 40, true});
    fa.add({"fleet_stacks_01.tga", 0, 0, 36, 45, true});
    fa.add({"fleet_empty.tga", 0, 0, 36, 45, true});
    GalacticHud hud;
    hud.init(ft, fa, env.fonts);
    hud.resize(1024, 768);
    GalacticCameraController cam;
    std::vector<GalacticPlanetInfo> planets(2);
    planets[0].planet = 100;
    planets[0].position = {200, 100, 0};
    planets[0].selected = true;
    planets[1].planet = 101;
    planets[1].position = {350, 200, 0};
    GalacticHudFrame gf;
    gf.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
    gf.planets = planets;
    gf.providers.fleets = [](uint32_t, std::vector<GalacticFleet>& out) {
        out.push_back({900, {{901, "i_frame.tga", 1, false}, {902, "i_frame.tga", 1, false}}});
        out.push_back({950, {{951, "i_frame.tga", 1, false}}});
    };
    gf.providers.land_forces = [](uint32_t, std::vector<GalacticFleetSlot>& out) { out.push_back({701, "i_frame.tga", 1, false}); };
    hud.build(gf);
    CHECK(hud.draggable(ft.find("q1_0000")) && hud.draggable(ft.find("q1_fleet")) && hud.draggable(ft.find("land_fleet_00")));
    auto center_px = [&](std::string_view name) {
        const int32_t c = ft.find(name);
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == c) return hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
        return Vec2f{-1, -1};
    };
    auto drag = [&](std::string_view from, Vec2f to, int32_t to_component) {
        const int32_t fc = ft.find(from);
        const Vec2f p = center_px(from);
        CHECK(p.x >= 0);
        std::vector<commands::EventData> out;
        RoutedInput r;
        r.world.mouse_x = static_cast<int32_t>(p.x);
        r.world.mouse_y = static_cast<int32_t>(p.y);
        r.hover_component = fc;
        r.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
        r.hud = {{HudPointerEvent::Kind::Press, Button::Left, fc, r.world.mouse_x, r.world.mouse_y}};
        hud.update_input(r, false, false, 1.0, out);
        r.hud.clear();
        r.world.mouse_x = static_cast<int32_t>(to.x);
        r.world.mouse_y = static_cast<int32_t>(to.y);
        r.hover_component = to_component;
        hud.update_input(r, false, false, 1.1, out);
        CHECK(hud.drag().active && hud.drag().component == fc);
        hud.build(gf);  // the dragged icon follows the cursor
        CHECK(!hud.draw_list().items(kHudLayers - 1, HudList::Alpha).empty());
        r.owner = {};
        r.hud = {{HudPointerEvent::Kind::Release, Button::Left, to_component, r.world.mouse_x, r.world.mouse_y}};
        hud.update_input(r, false, false, 1.2, out);
        CHECK(!hud.drag().active);
        hud.build(gf);
        return out;
    };
    // unit 902 of fleet 900 onto fleet 950's pad -> merge (op 3) {target 950, fleet 900, unit 902}
    std::vector<commands::EventData> o = drag("q1_0001", center_px("q2_0000"), ft.find("q2_0000"));
    CHECK(o.size() == 1 && o[0] == fleet_management_event(950, 900, 902, 3));
    // fleet 900 header onto the other planet -> MoveObjectToObject(900, 101)
    float sx = 0, sy = 0;
    CHECK(project(gf.view, planets[1].position, sx, sy));
    o = drag("q1_fleet", {sx, sy}, -1);
    CHECK(o.size() == 1 && o[0] == commands::ev::move_object_to_object(900, 101));
    // fleet header onto its own planet -> nothing; land unit onto a fleet -> launch (op 4)
    CHECK(project(gf.view, planets[0].position, sx, sy));
    CHECK(drag("q1_fleet", {sx, sy}, -1).empty());
    o = drag("land_fleet_00", center_px("q1_0000"), ft.find("q1_0000"));
    CHECK(o.size() == 1 && o[0] == fleet_management_event(900, commands::kNoObject, 701, 4));
    // unit into empty space -> detach (op 2) into a new fleet at the selected planet
    o = drag("q1_0000", {1000, 10}, -1);
    CHECK(o.size() == 1 && o[0] == fleet_management_event(100, commands::kNoObject, 901, 2));
    // a short press (no drag) still clicks: GalacticSelect
    std::vector<HudAction> ga;
    std::vector<commands::EventData> co;
    RoutedInput r;
    const int32_t fc = ft.find("q1_0000");
    r.hover_component = fc;
    r.hud = {{HudPointerEvent::Kind::Press, Button::Left, fc, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, fc, 0, 0},
             {HudPointerEvent::Kind::Click, Button::Left, fc, 0, 0}};
    hud.update_input(r, false, false, 2.0, co, &ga);
    CHECK(co.empty() && ga.size() == 1 && ga[0].kind == HudAction::Kind::GalacticSelect);

    // GALAXY_MAP_PRESENTATION Â§1 map fleet buttons (one g_planet_fleet component, one instance per slot): a left click
    // picks the instance under the cursor -> GalacticSelect {fleet}; right click nothing; an own fleet drags to another
    // planet -> MoveObjectToObject; an enemy fleet does not drag; a land-forces button is click-only.
    {
        planets[0].orbit_local = true;
        GalacticPlanetStack own;
        own.slot = 2;
        own.units = 3;
        own.fleet = 960;
        GalacticPlanetStack foe;
        foe.slot = 0;
        foe.units = 2;
        foe.enemy = true;
        foe.fleet = 990;
        GalacticPlanetStack none;  // no fleet id: drawn, not clickable
        none.slot = 1;
        none.units = 1;
        GalacticPlanetStack land;
        land.land = true;
        land.units = 4;
        land.fleet = 970;
        planets[0].stacks = {own, foe, none, land};
        hud.build(gf);
        const int32_t gpf = ft.find("g_planet_fleet"), gpl = ft.find("g_planet_land_forces");
        std::vector<Vec2f> fleet_px;  // hit centres (pixels) in draw order: slot 0 (foe), slot 2 (own)
        Vec2f land_px{-1, -1};
        for (const HudHitBox& h : hud.hit_boxes()) {
            const Vec2f c = hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
            if (h.component == gpf) fleet_px.push_back(c);
            if (h.component == gpl) land_px = c;
            if (h.component == gpf) CHECK(std::fabs(h.max.x - h.min.x - 36.0f) < 0.01f && std::fabs(h.max.y - h.min.y - 45.0f) < 0.01f);
        }
        CHECK(fleet_px.size() == 2 && land_px.x >= 0 && hud.draggable(gpf) && !hud.draggable(gpl));
        // Right presses on these buttons go to the world (planet picking / right-click move); left presses stay on the HUD.
        CHECK(!hud.takes_button(gpf, Button::Right) && !hud.takes_button(gpl, Button::Right) && hud.takes_button(gpf, Button::Left) &&
              hud.takes_button(ft.find("q1_0000"), Button::Right));
        {
            InputRouter router;
            RouteHooks hooks;
            hooks.hud_hit = [&](int32_t, int32_t) { return gpl; };
            hooks.hud_takes = [&](int32_t c, Button b) { return hud.takes_button(c, b); };
            InputFrame raw;
            raw.width = 1024;
            raw.height = 768;
            raw.buttons_down = button_bit(Button::Right);
            raw.buttons_pressed = button_bit(Button::Right);
            RoutedInput ro;
            router.route(raw, hooks, ro);
            CHECK(ro.owner[static_cast<size_t>(Button::Right)] == PointerTarget::World && (ro.world.buttons_pressed & button_bit(Button::Right)) &&
                  ro.hud.empty());
            raw.buttons_down = static_cast<uint8_t>(button_bit(Button::Right) | button_bit(Button::Left));
            raw.buttons_pressed = button_bit(Button::Left);
            router.route(raw, hooks, ro);
            CHECK(ro.owner[static_cast<size_t>(Button::Left)] == PointerTarget::Hud);
        }
        auto click_at = [&](int32_t comp, Vec2f px, Button b) {
            std::vector<HudAction> acts;
            std::vector<commands::EventData> cmds;
            RoutedInput ri;
            const auto x = static_cast<int32_t>(px.x), y = static_cast<int32_t>(px.y);
            ri.world.mouse_x = x;
            ri.world.mouse_y = y;
            ri.hover_component = comp;
            ri.hud = {{HudPointerEvent::Kind::Press, b, comp, x, y}, {HudPointerEvent::Kind::Release, b, comp, x, y},
                      {HudPointerEvent::Kind::Click, b, comp, x, y}};
            hud.update_input(ri, false, false, 3.0, cmds, &acts);
            CHECK(cmds.empty());
            return acts;
        };
        if (fleet_px.size() == 2) {
            std::vector<HudAction> a = click_at(gpf, fleet_px[1], Button::Left);
            CHECK(a.size() == 1 && a[0].kind == HudAction::Kind::GalacticSelect && a[0].ids == std::vector<uint32_t>{960} &&
                  a[0].unit == 100 && a[0].value == 2);
            a = click_at(gpf, fleet_px[0], Button::Left);
            CHECK(a.size() == 1 && a[0].ids == std::vector<uint32_t>{990} && a[0].value == 0);
            CHECK(click_at(gpf, fleet_px[1], Button::Right).empty());
            a = click_at(gpl, land_px, Button::Left);
            CHECK(a.size() == 1 && a[0].ids == std::vector<uint32_t>{970} && a[0].value == -1);
            // Drag the own fleet onto planet 101.
            auto drag_stack = [&](Vec2f from, Vec2f to) {
                std::vector<commands::EventData> out;
                RoutedInput rr;
                rr.world.mouse_x = static_cast<int32_t>(from.x);
                rr.world.mouse_y = static_cast<int32_t>(from.y);
                rr.hover_component = gpf;
                rr.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
                rr.hud = {{HudPointerEvent::Kind::Press, Button::Left, gpf, rr.world.mouse_x, rr.world.mouse_y}};
                hud.update_input(rr, false, false, 4.0, out);
                rr.hud.clear();
                rr.world.mouse_x = static_cast<int32_t>(to.x);
                rr.world.mouse_y = static_cast<int32_t>(to.y);
                rr.hover_component = -1;
                hud.update_input(rr, false, false, 4.1, out);
                const bool active = hud.drag().active;
                const bool ok = active && hud.drag().fleet == 960 && hud.drag().fleet_index == kMapFleetIndex && hud.drag().planet == 100 &&
                                hud.drag().valid;
                rr.owner = {};
                rr.hud = {{HudPointerEvent::Kind::Release, Button::Left, -1, rr.world.mouse_x, rr.world.mouse_y}};
                hud.update_input(rr, false, false, 4.2, out);
                hud.build(gf);
                return std::make_pair(active && ok, out);
            };
            float tx = 0, ty = 0;
            CHECK(project(gf.view, planets[1].position, tx, ty));
            auto [ok, out] = drag_stack(fleet_px[1], {tx, ty});
            CHECK(ok && out.size() == 1 && out[0] == commands::ev::move_object_to_object(960, 101));
            auto [ok2, out2] = drag_stack(fleet_px[0], {tx, ty});  // enemy fleet: no drag
            CHECK(!ok2 && out2.empty());
        }
        planets[0].stacks.clear();
        planets[0].orbit_local = false;
        hud.build(gf);
    }

    // Playtest 3 fleet slots (base 40.jpg): the free slots of every planet show as fleet_empty ovals with a fleet selected
    // or while dragging; drops on a slot issue FleetManagement / MoveObjectToObject (galactic_hud.hpp "Fleet-slot drops").
    {
        constexpr uint32_t kNone = commands::kNoObject;
        const int32_t gpf = ft.find("g_planet_fleet");
        planets[0].orbit_local = planets[1].orbit_local = true;
        GalacticPlanetStack a;  // the host feeds the sim's slots (playtest 3 t07); a drop only issues commands
        a.units = 3;
        a.fleet = 960;
        a.slot = 0;
        GalacticPlanetStack b2 = a;
        b2.fleet = 961;
        b2.slot = 1;
        planets[0].stacks = {a, b2};
        planets[1].stacks.clear();
        cam.set_look_at({275.0f, 150.0f, 0.0f});  // both planets on screen
        gf.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
        gf.show_fleet_slots = false;
        hud.build(gf);
        CHECK(!hud.stats().fleet_slots_shown && hud.stats().empty_slots == 0);
        gf.show_fleet_slots = true;
        hud.build(gf);
        CHECK(hud.stats().fleet_slots_shown && hud.stats().empty_slots == 1 + 3);
        // R5 (base 40.jpg): the empty ovals take the frame's tint (host: the local player's faction colour) at full alpha.
        gf.empty_slot_color = Rgba8{54, 134, 242, 255};
        hud.build(gf);
        int tinted = 0;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Alpha))
            if (q.color.r == 54 && q.color.g == 134 && q.color.b == 242 && q.color.a == 255) ++tinted;
        CHECK(tinted == 1 + 3);
        gf.empty_slot_color = Rgba8{};
        hud.build(gf);
        auto slot_px = [&](size_t pi, int k) {
            float sx = 0, sy = 0;
            CHECK(project(gf.view, planets[pi].position, sx, sy));
            const Vec2f at = hud.canvas().pixel_to_canvas(sx, sy);
            const Vec2f off[3] = {{-36, 42}, {0, 47}, {36, 42}};
            return hud.canvas().canvas_to_pixel(at.x + off[k].x, at.y + off[k].y);
        };
        // The fleet a click at a slot selects (0 = none / empty oval).
        auto fleet_at = [&](size_t pi, int k) -> uint32_t {
            const Vec2f p = slot_px(pi, k);
            std::vector<HudAction> acts;
            std::vector<commands::EventData> cmds;
            RoutedInput ri;
            const auto x = static_cast<int32_t>(p.x), y = static_cast<int32_t>(p.y);
            ri.world.mouse_x = x;
            ri.world.mouse_y = y;
            ri.hover_component = hud.hit_test(x, y);
            ri.hud = {{HudPointerEvent::Kind::Press, Button::Left, ri.hover_component, x, y},
                      {HudPointerEvent::Kind::Release, Button::Left, ri.hover_component, x, y},
                      {HudPointerEvent::Kind::Click, Button::Left, ri.hover_component, x, y}};
            hud.update_input(ri, false, false, 5.0, cmds, &acts);
            CHECK(cmds.empty());
            return acts.size() == 1 && acts[0].ids.size() == 1 && acts[0].value == k ? acts[0].ids[0] : 0u;
        };
        CHECK(hud.hit_test(static_cast<int32_t>(slot_px(0, 2).x), static_cast<int32_t>(slot_px(0, 2).y)) == gpf);  // empty oval
        CHECK(fleet_at(0, 0) == 960 && fleet_at(0, 1) == 961 && fleet_at(0, 2) == 0);
        // Map drag: press on a slot's fleet, move to `to`, release there (hover from the HUD hit test, as the router does).
        auto drag_map = [&](Vec2f from, Vec2f to, bool* valid) {
            std::vector<commands::EventData> out;
            RoutedInput rr;
            rr.world.mouse_x = static_cast<int32_t>(from.x);
            rr.world.mouse_y = static_cast<int32_t>(from.y);
            rr.hover_component = gpf;
            rr.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
            rr.hud = {{HudPointerEvent::Kind::Press, Button::Left, gpf, rr.world.mouse_x, rr.world.mouse_y}};
            hud.update_input(rr, false, false, 6.0, out);
            rr.hud.clear();
            rr.world.mouse_x = static_cast<int32_t>(to.x);
            rr.world.mouse_y = static_cast<int32_t>(to.y);
            rr.hover_component = -1;
            hud.update_input(rr, false, false, 6.1, out);  // drag starts
            hud.build(gf);                                   // slots shown while dragging, the source slot blank
            rr.hover_component = hud.hit_test(rr.world.mouse_x, rr.world.mouse_y);
            hud.update_input(rr, false, false, 6.2, out);
            if (valid) *valid = hud.drag().active && hud.drag().valid;
            rr.owner = {};
            rr.hud = {{HudPointerEvent::Kind::Release, Button::Left, rr.hover_component, rr.world.mouse_x, rr.world.mouse_y}};
            hud.update_input(rr, false, false, 6.3, out);
            hud.build(gf);
            return out;
        };
        // 1. Whole fleet onto an empty slot of its planet -> op 8 {planet, fleet, slot}; it is drawn there, the other stays.
        bool valid = false;
        std::vector<commands::EventData> so = drag_map(slot_px(0, 0), slot_px(0, 2), &valid);
        CHECK(valid && so.size() == 1 && so[0] == fleet_management_event(100, 960, kNone, 8, 2));
        CHECK(fleet_at(0, 0) == 960 && fleet_at(0, 2) == 0);  // the HUD keeps no slot state: nothing moved yet
        a.slot = 2;                                            // the sim applied op 8; the host feeds the new slot
        planets[0].stacks = {a, b2};
        hud.build(gf);
        CHECK(fleet_at(0, 2) == 960 && fleet_at(0, 1) == 961 && fleet_at(0, 0) == 0);
        // 2. Onto another own fleet's slot at the same planet -> merge (op 3) {target, source, all}.
        so = drag_map(slot_px(0, 2), slot_px(0, 1), &valid);
        CHECK(valid && so.size() == 1 && so[0] == fleet_management_event(961, 960, kNone, 3));
        // 3. Onto an empty slot of another planet -> op 8 {planet, fleet, slot, a = destination} (the slot it takes on
        //    arrival), then MoveObjectToObject {fleet, planet}.
        so = drag_map(slot_px(0, 2), slot_px(1, 1), &valid);
        CHECK(valid && so.size() == 2 && so[0] == fleet_management_event(100, 960, kNone, 8, 1, 101) &&
              so[1] == commands::ev::move_object_to_object(960, 101));
        // 4. An organiser unit onto an empty slot of its planet -> detach (op 2); the new fleet takes that slot (not the
        //    first free one).
        planets[0].stacks = {b2};  // 961 in slot 1; 0 and 2 free
        hud.build(gf);
        so = drag("q1_0001", slot_px(0, 2), gpf);
        CHECK(so.size() == 1 && so[0] == fleet_management_event(100, 900, 902, 2).set("b", 2u));  // the sim gives the slot
        GalacticPlanetStack c = a;
        c.fleet = 905;
        c.slot = 2;
        planets[0].stacks = {b2, c};
        hud.build(gf);
        CHECK(fleet_at(0, 2) == 905 && fleet_at(0, 1) == 961 && fleet_at(0, 0) == 0);
        // 5. A land unit onto the empty slot -> op 6 {planet, unit, a = planet} (launched as a new fleet).
        so = drag("land_fleet_00", slot_px(0, 0), gpf);
        CHECK(so.size() == 1 && so[0] == fleet_management_event(100, kNone, 701, 6, 0, 100).set("b", 0u));
        // 6. An enemy fleet's slot at the same planet: no drop.
        GalacticPlanetStack foe = a;
        foe.fleet = 990;
        foe.enemy = true;
        foe.slot = 0;
        planets[0].stacks = {b2, c, foe};
        hud.build(gf);
        CHECK(fleet_at(0, 0) == 990);
        so = drag_map(slot_px(0, 1), slot_px(0, 0), &valid);
        CHECK(!valid && so.empty());
        // 7. Hover highlight on the slot under the cursor while the slots are shown.
        gf.cursor_px = slot_px(1, 0);
        hud.build(gf);
        CHECK(hud.stats().slot_highlights == 1);
        gf.cursor_px = {};
        gf.show_fleet_slots = false;
        planets[0].stacks.clear();
        hud.build(gf);
        CHECK(!hud.stats().fleet_slots_shown && hud.stats().empty_slots == 0);
    }
}

// ---- [port: Gabriel] camera and controls: pause, FOV, free rotation, W A S D, queued orders ------------------------
InputFrame key_frame(std::initializer_list<uint16_t> down, std::initializer_list<uint16_t> pressed, double time) {
    InputFrame f;
    f.time = time;
    f.dt = 0.5f;
    f.width = 1024;
    f.height = 768;
    f.mouse_x = 512;
    f.mouse_y = 384;
    for (uint16_t k : down) f.keys_down.set(k);
    for (uint16_t k : pressed) {
        f.keys_down.set(k);
        f.keys_pressed.set(k);
    }
    return f;
}

void test_controls() {
    // 1. Pause: the port extension event round trip, the hotkey (Pause key) toggling, and both HUD buttons.
    bool pz = false;
    CHECK(read_pause_event(pause_event(true), pz) && pz && read_pause_event(pause_event(false), pz) && !pz);
    CHECK(!read_pause_event(commands::ev::stop(), pz));
    CHECK(pause_event(true).type() == commands::EventType::PortExtension && pause_event(true).u("subtype") == kPortPauseSubtype);
    {
        HotkeyTable table;
        CHECK(table.binding(hk::kPause).key == vk::kPause && kHotkeySlots == 162 && hotkey_slot("Pause") == hk::kPause);
        HotkeyDispatcher disp;
        InputState in;
        std::vector<commands::EventData> cmds;
        std::vector<HotkeyEvent> evs;
        in.update(key_frame({}, {vk::kPause}, 1.0));
        disp.update(in, table, {}, 1.0, cmds, evs);
        in.update(key_frame({}, {}, 1.1));
        in.update(key_frame({}, {vk::kPause}, 1.2));
        disp.update(in, table, {}, 1.2, cmds, evs);
        CHECK(cmds.size() == 2 && cmds[0] == pause_event(true) && cmds[1] == pause_event(false) && !disp.paused());
        Bindings b;
        table.apply_to_bindings(b);
        CHECK(b.binding(Action::Pause, 0).key == vk::kPause);
        Bindings d;  // P1 defaults have the action too
        CHECK(d.binding(Action::Pause, 0).key == vk::kPause && action_name(Action::Pause) == "Pause");
    }
    {
        ComponentTable ft;
        const data::XmlDocument fd = data::XmlDocument::parse(std::string_view(kFleetXml), "f.xml");
        ft.parse(fd.root());
        TextureAtlas fa;
        fa.texture_width = fa.texture_height = 64;
        fa.add({"i_big.tga", 0, 0, 30, 30, true});
        fa.add({"i_frame.tga", 0, 0, 40, 40, true});
        auto raster = make_box_rasterizer();
        FontCache fonts(*raster);
        auto click = [](auto& hud, int32_t ci, std::vector<commands::EventData>& out) {
            RoutedInput r;
            r.hover_component = ci;
            r.hud = {{HudPointerEvent::Kind::Press, Button::Left, ci, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, ci, 0, 0},
                     {HudPointerEvent::Kind::Click, Button::Left, ci, 0, 0}};
            hud.update_input(r, false, false, 1.0, out);
        };
        GalacticHud g;
        g.init(ft, fa, fonts);
        g.resize(1024, 768);
        GalacticHudFrame gf;
        gf.paused = true;
        g.build(gf);
        std::vector<commands::EventData> out;
        click(g, ft.find("b_play_pause"), out);
        CHECK(out.size() == 1 && out[0] == pause_event(false));  // paused -> resume
        TacticalHud t;
        t.init(ft, fa, fonts);
        t.resize(1024, 768);
        TacticalHudFrame tf;
        t.build(tf, {}, {});
        bool on_hud = false;
        for (const HudHitBox& h : t.hit_boxes()) on_hud = on_hud || h.component == ft.find("b_play_pause_t");
        CHECK(on_hud);
        out.clear();
        click(t, ft.find("b_play_pause_t"), out);
        CHECK(out.size() == 1 && out[0] == pause_event(true));
        // Queue markers through the tactical HUD frame.
        TacticalCameraController cam;
        tf.world.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
        HudOrderQueue q;
        q.unit = 7;
        q.position = {0, 0, 0};
        q.steps = {{HudQueuedStep::Kind::Attack, {200, 100, 0}, true},
                   {HudQueuedStep::Kind::Ability, {}, false},
                   {HudQueuedStep::Kind::Move, {-150, 250, 0}, true}};
        tf.order_queues = std::span<const HudOrderQueue>(&q, 1);
        t.build(tf, {}, {});
        CHECK(t.stats().queue_markers == 2 && t.stats().queue_dots > 10);
        // the markers are numbered 1 and 3 (the pointless ability step keeps its place in the count)
        HudDrawList dl;
        const QueueMarkerStats qs = add_queue_markers(dl, &fonts, t.canvas(), tf.world.view, tf.order_queues, 1);
        CHECK(qs.markers == 2 && dl.items(1, HudList::Text).size() >= 2);
        bool red = false;
        for (const HudQuad& qd : dl.items(1, HudList::Alpha)) red = red || qd.color == queue_step_color(HudQueuedStep::Kind::Attack);
        CHECK(red);
    }

    // 2. Field of view: tactical override, galactic setting, the options dialog.
    {
        TacticalCameraController tc;
        tc.set_fov_override(80.0f);
        CHECK(std::fabs(tc.params(4.0f / 3.0f).fov_x - 80.0f * kDegToRad) < 1e-5f && tc.fov_deg() == 80.0f);
        tc.set_fov_override(0.0f);
        CHECK(tc.fov_deg() == 55.0f);
        GalacticCameraController gc;
        CHECK(std::fabs(gc.fov_deg() - 26.565f) < 1e-2f);
        gc.set_fov_deg(40.0f);
        CHECK(std::fabs(gc.params(1.5f).fov_x - 40.0f * kDegToRad) < 1e-5f);
    }
    {
        Env env;
        CHECK(env.script.merge(kPortDialogsRc) && env.gui.has_dialog(kPortControlsDialog));
        FrontEnd fe;
        HotkeyTable keys;
        Bindings binds;
        MenuProviders p;
        p.hotkeys = &keys;
        p.bindings = &binds;
        CameraSettings cs;
        cs.tactical_fov_deg = 70.0f;
        p.camera_settings = [&] { return cs; };
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_MODS")}, InputFrame{}, acts);
        CHECK(fe.top() == kPortControlsDialog);
        GuiDialog* d = env.gui.top();
        CHECK(d && d->get("IDC_PORT_TACTICAL_FOV_SLIDER")->value == 70 && d->get("IDC_PORT_GALACTIC_FOV_SLIDER")->value == 27 &&
              d->get("IDC_PORT_TACTICAL_FOV_SLIDER")->range_max == 110 && d->get("IDC_EDIT_PORT_PAUSE")->text == U"Pause" &&
              d->get("IDC_EDIT_PORT_SCROLL_UP")->text == U"W" && d->get("IDC_PORT_QUEUE_MODIFIER")->selected == 0);
        // Slider moves update the value label; key capture rebinds Pause to F9; the queue modifier becomes Ctrl.
        d->set_value("IDC_PORT_GALACTIC_FOV_SLIDER", 35);
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, kPortControlsDialog, "IDC_PORT_GALACTIC_FOV_SLIDER", 35)},
                  InputFrame{}, acts);
        CHECK(d->get("IDC_PORT_GALACTIC_FOV_VALUE")->text == U"35");
        d->focus("IDC_EDIT_PORT_PAUSE");
        InputFrame kf;
        kf.keys_pressed.set(0x78);  // F9
        kf.keys_down.set(0x78);
        fe.update({}, kf, acts);
        CHECK(d->get("IDC_EDIT_PORT_PAUSE")->text == U"F9");
        d->set_selected("IDC_PORT_QUEUE_MODIFIER", 1);
        acts.clear();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, kPortControlsDialog, "IDC_BUTTON_ACCEPT")}, InputFrame{}, acts);
        CHECK(acts.size() == 2 && acts[0].kind == MenuAction::Kind::ApplyCamera && acts[0].camera.galactic_fov_deg == 35.0f &&
              acts[0].camera.tactical_fov_deg == 70.0f && acts[0].camera.queue_mods == kModCtrl &&
              acts[1].kind == MenuAction::Kind::KeybindingsChanged);
        CHECK(keys.binding(hk::kPause).key == 0x78 && binds.binding(Action::Pause, 0).key == 0x78 && binds.queue_mods() == kModCtrl);
        CHECK(env.gui.find(kPortControlsDialog) == nullptr);
    }
    // t36 (QA V6): every way out of Camera and Controls lands on the Options page that opened it, not the pause menu. Real
    // mouse clicks and Esc through the GUI; the pause menu stays underneath.
    {
        Env env;
        CHECK(env.script.merge(kPortDialogsRc));
        CHECK(env.script.merge("IDD_GAME_OPTIONS_DIALOG DIALOGEX 0, 0, 321, 328\nFONT 8, \"Arial\", 0, 0, 0x0\nBEGIN\n"
                               "    PUSHBUTTON \"o\",IDC_INGAME_OPTIONS,10,100,100,20\nEND\n"
                               "IDD_IN_GAME_OPTIONS_DIALOG DIALOGEX 0, 0, 321, 328\nFONT 8, \"Arial\", 0, 0, 0x0\nBEGIN\n"
                               "    PUSHBUTTON \"n\",IDC_IN_GAME_OPTIONS_NETWORK,10,100,100,20\n"
                               "    PUSHBUTTON \"b\",IDC_OK_IN_GAME,10,200,100,20\nEND\n"));
        FrontEnd fe;
        HotkeyTable keys;
        Bindings binds;
        MenuProviders p;
        p.hotkeys = &keys;
        p.bindings = &binds;
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.show_pause_menu();
        fe.update(env.click("IDD_GAME_OPTIONS_DIALOG", "IDC_INGAME_OPTIONS"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        const auto open_controls = [&] {
            fe.update(env.click("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_IN_GAME_OPTIONS_NETWORK"), InputFrame{}, acts);
            CHECK(fe.top() == kPortControlsDialog);
        };
        open_controls();
        fe.update(env.click(kPortControlsDialog, "IDC_BUTTON_ACCEPT"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        fe.update(env.frame({1.0f, 1.0f}, false), InputFrame{}, acts);  // the frame after: nothing more closes
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        open_controls();
        fe.update(env.click(kPortControlsDialog, "IDC_CANCEL_BUTTON"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG");
        open_controls();
        fe.update(env.frame({1.0f, 1.0f}, false, {vk::kEscape}), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_IN_GAME_OPTIONS_DIALOG" && fe.pause_menu_open());
        fe.update(env.click("IDD_IN_GAME_OPTIONS_DIALOG", "IDC_OK_IN_GAME"), InputFrame{}, acts);
        CHECK(fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    }

    // 2b. Gameplay options defaults (P1_CAMERA_INPUT.md Â§3.1): absolute scrolling on, scroll speed 1.0 at the default
    //     slider position; the Defaults button restores them. GalacticCameraController::set_tuning keeps the pose.
    {
        const GameplaySettings g;
        CHECK(g.absolute_scroll && !g.right_button_scroll && !g.both_buttons_rotate && g.scroll_speed == kDefaultScrollSpeedSlider);
        CHECK(scroll_speed_option_from_slider(kDefaultScrollSpeedSlider) == 1.0f && scroll_speed_option_from_slider(0) == 0.0f &&
              scroll_speed_option_from_slider(100) == 3.0f && scroll_speed_option_from_slider(500) == 3.0f &&
              scroll_speed_option_from_slider(66) > 1.9f && scroll_speed_option_from_slider(66) < 2.1f);
        Env env;
        CHECK(env.script.merge(R"RC(
IDD_GAMEPLAY_OPTIONS_DIALOG DIALOGEX 0, 0, 400, 200
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "",IDC_SCROLL_SPEED_SLIDER,"msctls_trackbar32",TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,10,10,200,18
    CONTROL         "Check",IDC_ABSOLUTE_SCROLLING,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,10,40,100,12
    PUSHBUTTON      "TEXT_BUTTON_DEFAULTS",IDC_DEFAULTS_BUTTON,10,160,160,24
END
)RC"));
        FrontEnd fe;
        MenuProviders p;
        GameplaySettings cur;
        cur.scroll_speed = 90;
        cur.absolute_scroll = false;
        p.gameplay = [&] { return cur; };
        fe.init(env.gui, {}, p);
        std::vector<MenuAction> acts;
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_GAMEPLAY")}, InputFrame{}, acts);
        GuiDialog* d = env.gui.top();
        CHECK(d && d->name() == "IDD_GAMEPLAY_OPTIONS_DIALOG");
        if (d && d->name() == "IDD_GAMEPLAY_OPTIONS_DIALOG") {
            CHECK(!d->get("IDC_ABSOLUTE_SCROLLING")->checked && d->get("IDC_SCROLL_SPEED_SLIDER")->value == 90);
            fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_GAMEPLAY_OPTIONS_DIALOG", "IDC_DEFAULTS_BUTTON")},
                      InputFrame{}, acts);
            CHECK(d->get("IDC_ABSOLUTE_SCROLLING")->checked && d->get("IDC_SCROLL_SPEED_SLIDER")->value == kDefaultScrollSpeedSlider);
        }
        GalacticCameraController gc;
        gc.set_bounds({-100, -100, 100, 100});
        gc.set_look_at({30, 40, 0});
        gc.set_fov_deg(40.0f);
        CameraTuning tu;
        tu.scroll_speed_option = 2.0f;
        tu.absolute_scrolling = false;
        gc.set_tuning(tu);
        CHECK(gc.tuning().scroll_speed_option == 2.0f && !gc.tuning().absolute_scrolling && gc.look_at().x == 30.0f &&
              gc.look_at().y == 40.0f && gc.fov_deg() == 40.0f && gc.limits().max_x == 115.0f && gc.zoom_state() == 0);
    }

    // 3. Free tactical camera: no clamps on yaw / pitch, roll, keys on every axis, consistent picking view.
    {
        TacticalCameraController c;  // Space_Mode, free rotation by default
        c.rotate(0.0f, 100.0f);
        CHECK(std::fabs(c.pitch_deg() - 150.0f) < 1e-3f);  // past the vertical (data max 85)
        c.rotate(400.0f, 0.0f);
        CHECK(std::fabs(c.yaw_deg() - 40.0f) < 1e-3f);  // wrapped, not clamped at 1000
        c.roll(30.0f);
        CameraParams p = c.params(4.0f / 3.0f);
        CHECK(std::fabs(p.roll - 30.0f * kDegToRad) < 1e-5f && !(p.up.x == 0.0f && p.up.y == 0.0f && p.up.z == 1.0f));
        // up is perpendicular to the view direction and unit length
        const Vec3 v{p.target.x - p.eye.x, p.target.y - p.eye.y, p.target.z - p.eye.z};
        const float vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        CHECK(std::fabs((v.x * p.up.x + v.y * p.up.y + v.z * p.up.z) / vl) < 1e-4f &&
              std::fabs(p.up.x * p.up.x + p.up.y * p.up.y + p.up.z * p.up.z - 1.0f) < 1e-4f);
        // the target projects to the screen centre; a point along +up projects above it
        const View view = make_view(p, 1024, 768);
        float sx = 0, sy = 0, ux = 0, uy = 0;
        CHECK(project(view, p.target, sx, sy) && std::fabs(sx - 512.0f) < 0.01f && std::fabs(sy - 384.0f) < 0.01f);
        CHECK(project(view, {p.target.x + p.up.x * 50, p.target.y + p.up.y * 50, p.target.z + p.up.z * 50}, ux, uy) && uy < sy - 10.0f &&
              std::fabs(ux - sx) < 1.0f);
        c.reset();
        CHECK(c.roll_deg() == 0.0f && c.pitch_deg() == 50.0f);
        // Land_Mode locks the pitch in data; the free camera pitches with the keys / roll keys.
        TacticalCameraController land(land_mode_defaults());
        Bindings b;
        InputState in;
        in.update(key_frame({vk::kUp}, {}, 1.0));
        land.update(in, b, 0.5f);  // 60 deg/s * 0.5 s on top of the spline pitch 55 (fraction 1 from the start)
        CHECK(std::fabs(land.pitch_deg() - 85.0f) < 1e-3f);
        in.update(key_frame({vk::kNumpad6}, {}, 1.5));
        land.update(in, b, 0.5f);
        CHECK(std::fabs(land.roll_deg() - 30.0f) < 1e-3f);
        // the original clamps stay available
        CameraTuning clamped;
        clamped.free_rotation = false;
        TacticalCameraController old(space_mode_defaults(), UiConstants{}.tactical, clamped);
        old.rotate(0.0f, 100.0f);
        old.roll(10.0f);
        CHECK(old.pitch_deg() == 85.0f && old.roll_deg() == 0.0f);
    }

    // 4. W A S D: P1 defaults and the hotkey table; displaced hotkeys; modified combinations keep their meaning.
    {
        Bindings b;
        InputState in;
        TacticalCameraController c;
        in.update(key_frame({'W'}, {}, 1.0));
        c.update(in, b, 0.5f);
        CHECK(c.look_at().y > 100.0f && std::fabs(c.look_at().x) < 1e-3f);  // forward (yaw 0 = +Y)
        c.set_look_at({0, 0, 0});
        in.update(key_frame({vk::kShift, 'A'}, {}, 1.5));  // any modifier: Shift+A still scrolls left
        c.update(in, b, 0.5f);
        CHECK(c.look_at().x < -100.0f);
        CHECK(b.binding(Action::Stop, 0).key == 'H' && b.binding(Action::SelectAll, 0).key == 'Y' &&
              b.binding(Action::SelectAll, 0).mods == kModCtrl);
        // P1 Bindings: no action other than movement on W / A / S / D
        for (size_t a = 0; a < kActionCount; ++a) {
            const auto act = static_cast<Action>(a);
            const bool move = act == Action::ScrollUp || act == Action::ScrollDown || act == Action::ScrollLeft || act == Action::ScrollRight;
            for (size_t s = 0; s < Bindings::kSlots; ++s) {
                const uint16_t k = b.binding(act, s).key;
                CHECK(move || !(k == 'W' || k == 'A' || k == 'S' || k == 'D'));
            }
        }
        HotkeyTable t;
        CHECK(t.binding(hk::kScrollUp) == (HotkeyBinding{'W', HotkeyMod::None}) && t.binding(hk::kScrollLeft) == (HotkeyBinding{'A', HotkeyMod::None}) &&
              t.binding(hk::kScrollDown) == (HotkeyBinding{'S', HotkeyMod::None}) && t.binding(hk::kScrollRight) == (HotkeyBinding{'D', HotkeyMod::None}));
        CHECK(t.binding(hk::kAttack).key == 'X' && t.binding(hk::kStop).key == 'H' && t.binding(hk::kPrevUnit).key == 'V' &&
              t.binding(hk::kWaypoint).key == 'L');
        // no other slot uses a plain W / A / S / D; every default binding is unique
        bool unique = true, wasd_only_scroll = true;
        for (uint16_t i = 0; i < kHotkeySlots; ++i) {
            const HotkeyBinding& bi = t.binding(i);
            if (bi.key < 0) continue;
            for (uint16_t j = static_cast<uint16_t>(i + 1); j < kHotkeySlots; ++j) unique = unique && !(t.binding(j) == bi);
            const bool wasd = bi.key == 'W' || bi.key == 'A' || bi.key == 'S' || bi.key == 'D';  // any modifier
            if (wasd && i != hk::kScrollUp && i != hk::kScrollDown && i != hk::kScrollLeft && i != hk::kScrollRight) wasd_only_scroll = false;
        }
        CHECK(unique && wasd_only_scroll);
        // Keymap conflicts (P host open item): after the sync no key + modifier drives two Bindings actions, End / Home
        // only reset / home the camera (hotkey slots 73 / 75), zoom is on = / - (slots 160 / 161), defaults == synced.
        {
            Bindings synced;
            t.apply_to_bindings(synced);
            Bindings defaults;
            bool no_clash = true, same = true;
            for (size_t a = 0; a < kActionCount; ++a)
                for (size_t s = 0; s < Bindings::kSlots; ++s) {
                    const KeyBinding& ka = synced.binding(static_cast<Action>(a), s);
                    if (ka.key == 0) continue;
                    for (size_t a2 = a + 1; a2 < kActionCount; ++a2)
                        for (size_t s2 = 0; s2 < Bindings::kSlots; ++s2) {
                            const KeyBinding& kb2 = synced.binding(static_cast<Action>(a2), s2);
                            const bool any = (ka.mods & kModAny) != 0 || (kb2.mods & kModAny) != 0;
                            if (kb2.key == ka.key && (any || kb2.mods == ka.mods)) no_clash = false;
                        }
                }
            for (Action a : {Action::ZoomIn, Action::ZoomOut, Action::ResetCamera})
                same = same && synced.binding(a, 0) == defaults.binding(a, 0) && defaults.binding(a, 1).key == 0;
            CHECK(no_clash && same);
            CHECK(synced.binding(Action::ZoomIn, 0) == (KeyBinding{vk::kOemPlus, kModNone}) &&
                  synced.binding(Action::ZoomOut, 0) == (KeyBinding{vk::kOemMinus, kModNone}) &&
                  synced.binding(Action::ResetCamera, 0).key == vk::kEnd && t.binding(hk::kCameraHome).key == vk::kHome &&
                  t.binding(hk::kZoomIn).key == vk::kOemPlus && hotkey_slot("CameraZoomOut") == hk::kZoomOut);
            for (Action a : {Action::ZoomIn, Action::ZoomOut})
                for (size_t s = 0; s < Bindings::kSlots; ++s)
                    CHECK(synced.binding(a, s).key != vk::kHome && synced.binding(a, s).key != vk::kEnd);
        }
        // An original 153-slot profile with the original defaults: scroll arrows -> W A S D, every W / A / S / D use moves
        // to its port default (Attack A -> X, Ctrl+A -> Ctrl+Y, Shift+W -> Shift+Q, Alt+D -> Alt+\ ...), the arrows stay on
        // the port yaw / pitch slots.
        HotkeyProfile prof;
        {
            HotkeyTable orig;
            const std::pair<const char*, HotkeyBinding> original[] = {
                {"ScrollUp", {vk::kUp, HotkeyMod::None}},          {"ScrollDown", {vk::kDown, HotkeyMod::None}},
                {"ScrollLeft", {vk::kLeft, HotkeyMod::None}},      {"ScrollRight", {vk::kRight, HotkeyMod::None}},
                {"Attack", {'A', HotkeyMod::None}},                {"Stop", {'S', HotkeyMod::None}},
                {"PreviousUnit", {'D', HotkeyMod::None}},          {"Waypoint", {'W', HotkeyMod::None}},
                {"SelectAll", {'A', HotkeyMod::Ctrl}},             {"SuperWeapon", {'W', HotkeyMod::Ctrl}},
                {"Ability_Deploy", {'D', HotkeyMod::Ctrl}},        {"Ability_StimPack", {'S', HotkeyMod::Ctrl}},
                {"Ability_SpoilerLock", {'W', HotkeyMod::Shift}},  {"Ability_DestroyPlanet", {'D', HotkeyMod::Shift}},
                {"Ability_SuperLaser", {'A', HotkeyMod::Shift}},   {"Ability_SpaceSelfDestruct", {'S', HotkeyMod::Shift}},
                {"Ability_ReplenishWingmen", {'W', HotkeyMod::Alt}}, {"Ability_ForceSight", {'S', HotkeyMod::Alt}},
                {"Ability_DrainLife", {'D', HotkeyMod::Alt}},      {"Ability_FoCFullSalvo", {'A', HotkeyMod::Alt}}};
            for (const auto& [name, kb] : original) orig.bind(static_cast<uint16_t>(hotkey_slot(name)), kb);
            orig.save(prof);
        }
        prof["KeySettingsCount"] = kOriginalHotkeySlots;
        HotkeyTable loaded;
        CHECK(loaded.load(prof));
        bool migrated = true;
        for (uint16_t i = 0; i < kHotkeySlots; ++i) migrated = migrated && loaded.binding(i) == t.binding(i);
        CHECK(migrated);  // == the new defaults
        // a customised scroll key is kept; a port slot it collides with is unbound
        HotkeyTable custom;
        custom.bind(hk::kScrollUp, {vk::kNumpad4, HotkeyMod::None});
        HotkeyProfile cprof;
        custom.save(cprof);
        cprof["KeySettingsCount"] = kOriginalHotkeySlots;
        HotkeyTable cl;
        CHECK(cl.load(cprof) && cl.binding(hk::kScrollUp).key == vk::kNumpad4 && cl.binding(hk::kRollLeft).key == -1 &&
              cl.binding(hk::kYawLeft).key == vk::kLeft);
    }

    // 5. Queued orders: right click with the queue modifier (Shift; rebindable) sets the flag, a plain click clears it.
    {
        GalacticCameraController gcam;
        OrderContext ctx;
        ctx.mode = UiMode::Galactic;
        ctx.view = make_view(gcam.params(4.0f / 3.0f), 1024, 768);
        const PickSphere planet{77, {0, 0, 0}, 40.0f, -1, PickKind::Planet};
        ctx.galactic_items = std::span<const PickSphere>(&planet, 1);
        float px = 0, py = 0;
        CHECK(project(ctx.view, planet.center, px, py));
        OrderTranslator ot;
        ot.set_selection({5});
        Bindings b;
        InputState in;
        auto right_click = [&](std::initializer_list<uint16_t> mods, double t) {
            InputFrame f = key_frame(mods, {}, t);
            f.mouse_x = static_cast<int32_t>(px);
            f.mouse_y = static_cast<int32_t>(py);
            f.buttons_down = button_bit(Button::Right);
            f.buttons_pressed = button_bit(Button::Right);
            in.update(f);
            ot.update(in, b, ctx);
            f.buttons_down = 0;
            f.buttons_pressed = 0;
            f.buttons_released = button_bit(Button::Right);
            f.time = t + 0.05;
            in.update(f);
            ot.update(in, b, ctx);
        };
        right_click({vk::kShift}, 1.0);
        right_click({}, 3.0);
        CHECK(ot.pending().size() == 2 && ot.pending_queued() == (std::vector<uint8_t>{1, 0}));
        std::vector<UiOrder> orders = ot.take_orders();
        CHECK(orders.size() == 2 && orders[0].queued && !orders[1].queued &&
              orders[0].event == commands::ev::move_object_to_object(5, 77) && ot.pending().empty());
        b.set_queue_mods(kModCtrl);  // rebound
        right_click({vk::kShift}, 5.0);
        right_click({vk::kControl}, 7.0);
        orders = ot.take_orders();
        CHECK(orders.size() == 2 && !orders[0].queued && orders[1].queued);
        b.set_queue_mods(0);
        right_click({vk::kControl}, 9.0);
        CHECK(ot.take_orders().size() == 1 && !b.queue_held(in));
    }
}

// ---- camera hotkeys --------------------------------------------------------------------------------------------
void test_camera_hotkeys() {
    TacticalCameraController cam(space_mode_defaults());
    cam.set_bounds({-3000, -3000, 3000, 3000});
    std::vector<HotkeyEvent> ev(4);
    ev[0].slot = hk::kTacticalOverview;
    ev[0].kind = HotkeyKind::Camera;
    ev[1].slot = 2;  // second recall of group 3
    ev[1].kind = HotkeyKind::SelectGroup;
    ev[1].group = 3;
    ev[1].camera_jump = true;
    ev[2].slot = hk::kScreenshot;
    ev[2].kind = HotkeyKind::Interface;
    ev[3].slot = hk::kCameraCentre;
    ev[3].kind = HotkeyKind::Camera;
    CameraFocus focus;
    focus.group = [](int8_t g) -> std::optional<Vec3> {
        if (g != 3) return std::nullopt;
        const Vec3 pts[2] = {{100, 200, 0}, {300, 400, 0}};
        return focus_point(pts);
    };
    const std::vector<uint16_t> handled = apply_camera_hotkeys(ev, cam, focus);
    CHECK(handled.size() == 3 && handled[0] == hk::kTacticalOverview && handled[1] == 2 && handled[2] == hk::kCameraCentre);
    CHECK(cam.overview_level() == 1 && cam.look_at().x == 200.0f && cam.look_at().y == 300.0f);
    // HotkeyDispatcher marks the second recall within 1 s.
    HotkeyTable table;
    HotkeyDispatcher disp;
    InputState in;
    InputFrame f;
    std::vector<commands::EventData> cmds;
    std::vector<HotkeyEvent> out;
    for (int k = 0; k < 4; ++k) {
        f.time = 1.0 + 0.1 * k;
        f.keys_pressed = {};
        f.keys_down = {};
        if (k % 2 == 0) {
            f.keys_pressed.set('3');
            f.keys_down.set('3');
        }
        in.update(f);
        disp.update(in, table, {}, f.time, cmds, out);
    }
    CHECK(out.size() == 1 && out[0].camera_jump && out[0].group == 3 && cmds.size() == 2);
}

#ifdef ALAMO_UI_HAVE_VFS
void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real screens: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    GuiData gd;
    CHECK(load_gui_data(*files, gd, &err));
    CHECK(gd.script.merge(kPortDialogsRc) && gd.script.find(kPortControlsDialog) != nullptr && gd.script.warnings.empty());
    TextDatabase text;
    load_master_text(*files, "ENGLISH", text);
    TextureAtlas atlas;
    auto raster = make_box_rasterizer();
    FontCache fonts(*raster);
    GuiManager gui;
    gui.init(gd.script, gd.skin, atlas, fonts, &text);
    gui.resize(1920, 1080);
    {  // [port: Gabriel] the camera and controls dialog with the shipped skin; every key edit maps to a slot
        GuiDialog* pd = gui.open(kPortControlsDialog);
        size_t edits = 0, mapped = 0;
        for (size_t i = 0; pd && i < pd->size(); ++i)
            if ((*pd)[i].type == GuiGadgetType::EditBox) {
                ++edits;
                mapped += keyboard_edit_slot((*pd)[i].id()) >= 0 ? 1u : 0u;
            }
        CHECK(pd && edits == 13 && mapped == 13);  // + zoom in / out (slots 160 / 161)
        gui.close(kPortControlsDialog);
    }
    // Every in-game screen opens and fills on the shipped script.
    GameScreens gs;
    gs.init(gui);
    for (size_t n = 0; n <= 3; ++n) {
        MessageBoxSpec m;
        m.caption = U"Caption";
        for (size_t k = 0; k < n; ++k) m.buttons.push_back(U"B");
        CHECK(gs.show_message(m));
        m.centered = true;
        CHECK(gs.show_message(m));
        std::printf("real screens: %zu-button box -> %s\n", n, gs.message_dialog().c_str());
    }
    MessageBoxSpec small;
    small.small = true;
    CHECK(gs.show_message(small) && gs.message_dialog() == "IDD_DIALOG_BOX_SMALL");
    BattleEndInfo be;
    for (int i = 0; i < 14; ++i) be.your_losses.push_back({"i_button_tie.tga", U"TIE", 2});
    be.your_heroes = {"i_button_vader.tga"};
    CHECK(gs.show_battle_end(be));
    CHECK(gui.find(dlg::kBattleEnd)->get("IDC_YOUR_UNIT_NAME11")->visible && gui.find(dlg::kBattleEnd)->get("IDC_YOUR_HERO0")->visible &&
          !gui.find(dlg::kBattleEnd)->get("IDC_YOUR_HERO1")->visible);
    CampaignScoreInfo cs;
    cs.military = 87;
    cs.list = {U"Captured Coruscant"};
    CHECK(gs.show_campaign_score(cs) && gui.find(dlg::kCampaignScore)->get("IDC_MILITARY_EFFICIENCY_STATIC")->text == U"87%");
    CHECK(gs.show_retreat_confirm() && gs.open_chat());
    PlanetInfoData pi;
    pi.name = U"Tatooine";
    pi.history = {U"Outer Rim desert world."};
    CHECK(gs.show_planet_info(pi));
    AutoResolveInfo ar;
    ar.invaders = {U"Star Destroyer"};
    CHECK(gs.show_auto_resolve(ar));
    gs.set_auto_resolve_progress(0.4f, 0.6f);
    CHECK(gui.find(dlg::kAutoResolve)->get("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER")->value == 60);
    const SkirmishMapInfo maps[1] = {{"_SPACE_PLANET_KUAT_01", U"Kuat", 2}};
    CHECK(gs.show_select_map(maps));
    std::printf("real screens: open %zu dialogs\n", gui.open_count());
    gs.close_all();
    CHECK(gui.open_count() == 0);
    // Multiplayer screens on the shipped script.
    LobbyScreens lob;
    lob.init(gui);
    lob.show_multiplayer_menu();
    CHECK(lob.top() == dlg::kMultiplayer);
    lob.show_lan_lobby();
    CHECK(lob.top() == dlg::kLanLobby);
    StagingState st;
    st.host = true;
    st.slots.resize(9);
    st.slots[0].state = SlotState::Human;
    st.slots[0].local = true;
    st.slots[0].name = U"Gabriel";
    CHECK(lob.show_staging(st) && lob.top() == dlg::kLanStaging);
    st.internet = true;
    CHECK(lob.show_staging(st) && lob.top() == dlg::kInternetStaging);
    lob.show_internet_home();
    CHECK(lob.top() == dlg::kInternetHome);
    lob.set_internet_stats(12, 3, 4);
    lob.show_internet_lobby();
    CHECK(lob.top() == dlg::kInternetLobby);
    lob.show_direct_connect({U"192.168.1.20"});
    lob.show_password_prompt("g");
    lob.show_connecting();
    const ConnectionRow rows[1] = {{U"Host", 0.5f, true}};
    lob.show_disconnect(rows);
    lob.show_vote({});
    for (const char* n : {dlg::kDirectConnect, dlg::kPassword, dlg::kConnect, dlg::kDisconnect, dlg::kVote}) CHECK(gui.find(n) != nullptr);
    lob.close_all();
    CHECK(gui.open_count() == 0);
    // P4 completion 2: the remaining screens on the shipped script.
    {
        InfoScreens is;
        is.init(gui);
        CHECK(is.show_buy_units({1000, {{"X", U"X", 10, 0, "", true}}}) && is.show_story({}) && is.show_story_summary({}) &&
              is.show_planet({}));
        // GALACTIC_DIALOGS Â§2.2: chapter TITLE in the IDC_STATIC above the recap, newest chapter preselected, Completed
        // disabled with nothing completed, Abandon hidden.
        {
            StorySummaryInfo ss;
            ss.goals = {U"Chapter one", U"Heart of the Maw"};
            ss.recap = {U"Body", U"Empire Will Achieve Victory If:"};
            ss.recap_colors = {Rgba8{255, 255, 255, 176}, Rgba8{255, 24, 39, 255}};
            ss.title = U"Incoming Transmission from Darth Vader";
            ss.has_completed = false;
            CHECK(is.show_story_summary(ss));
            const GuiDialog* d = gui.find(dlg::kStorySummary);
            CHECK(d != nullptr);
            if (d) {
                bool title = false;
                for (size_t i = 0; i < d->size(); ++i)
                    title = title || ((*d)[i].def && (*d)[i].def->id == "IDC_STATIC" && (*d)[i].text == ss.title);
                const GuiControl* goals = d->get("IDC_GOAL_LIST");
                const GuiControl* done = d->get("IDC_RADIO_COMPLETED");
                const GuiControl* ab = d->get("IDC_SUMMARY_DIALOG_ABANDON");
                const GuiControl* recap = d->get("IDC_STORY_RECAP");
                CHECK(title && goals && goals->selected == 1 && done && !done->enabled && (!ab || !ab->visible) && recap &&
                      !recap->item_text_colors.empty() && recap->item_text_colors.back().r == 255 &&
                      recap->item_text_colors.back().g == 24);
            }
        }
        size_t tech_items = 0;
        TechTreeInfo tt;
        tt.item = [&](std::string_view, TechTreeItem& o) {
            ++tech_items;
            o.icon = "i_x.tga";
            return true;
        };
        for (TechFaction f : {TechFaction::Rebel, TechFaction::Empire, TechFaction::BlackMarket})
            for (bool space : {false, true}) {
                tt.faction = f;
                tt.space = space;
                is.set_tech_tree(tt);
                CHECK(is.show_info(InfoPage::TechTree) && is.info_dialog() == tech_tree_dialog(f, space));
            }
        for (InfoPage p : {InfoPage::DroidLog, InfoPage::DataChart, InfoPage::SummaryProduction, InfoPage::SummaryFleets,
                           InfoPage::SummaryEconomy})
            CHECK(is.show_info(p));
        std::printf("real info screens: %zu tech tree items over the 6 tech tree dialogs\n", tech_items);
        CHECK(tech_items > 100);
        is.close_all();
        CHECK(gui.open_count() == 0);
    }
    // Mouse pointers: MousePointers.xml + frame textures in the game data.
    {
        std::vector<uint8_t> bytes;
        CHECK(files->read("Data\\XML\\MousePointers.xml", bytes));
        const data::XmlDocument doc = data::XmlDocument::parse(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "MousePointers.xml");
        CursorTable ct;
        const size_t n = ct.parse(doc.root());
        size_t known = 0;
        for (const CursorDef& d : ct.defs()) {
            Pointer p;
            known += pointer_from_name(d.name, p) ? 1u : 0u;
        }
        const size_t frames = ct.resolve_frames([&](std::string_view t) {
            const std::string s(t);
            return files->exists("Data\\Art\\Textures\\" + s) ||
                   files->exists("Data\\Art\\Textures\\" + s.substr(0, s.find_last_of('.')) + ".dds");
        });
        std::printf("real cursor: %zu pointers (%zu known), %zu frames\n", n, known, frames);
        CHECK(n == kPointerCount && known == kPointerCount && frames > n);
    }
    // Every panel component resolves in the shipped CommandBarComponents.
    ComponentTable table;
    CHECK(table.load(*files, &err));
    HeroPanel hp;
    BlackMarketPanel bp;
    SabotagePanel sp;
    CorruptionPanel cp;
    CHECK(hp.init(table, atlas, fonts) && bp.init(table, atlas, fonts) && sp.init(table, atlas, fonts) && cp.init(table, atlas, fonts));
    size_t missing = 0;
    auto need = [&](const std::string& n) {
        if (table.find(n) < 0) {
            ++missing;
            std::printf("  missing component %s\n", n.c_str());
        }
    };
    for (const char* s : {"good", "evil"})
        for (int k = 0; k <= 10; ++k) {
            char b[40];
            std::snprintf(b, sizeof b, "hero_slot_%s%02d", s, k);
            need(b);
            std::snprintf(b, sizeof b, "hero_hb_%s%02d", s, k);
            need(b);
        }
    for (int k = 0; k < 5; ++k) {
        char b[40];
        std::snprintf(b, sizeof b, "market_%02d", k);
        need(b);
        std::snprintf(b, sizeof b, "market_tech_level%02d", k);
        need(b);
    }
    for (int k = 0; k < 14; ++k) {
        char b[40];
        std::snprintf(b, sizeof b, "market_title%02d", k);
        need(b);
    }
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 2; ++c) {
            char b[40];
            std::snprintf(b, sizeof b, "bm_%02d0%d", r, c);
            need(b);
            std::snprintf(b, sizeof b, "bm_title_%d0%d", r, c);
            need(b);
            std::snprintf(b, sizeof b, "bm_title_40%d", r * 2 + c + 2);
            need(b);
        }
    for (const char* x : {"a", "b", "c"}) {
        need(std::string("corrupt_button_icon_") + x);
        need(std::string("corrupt_title_") + x);
        need(std::string("corrupt_cost_") + x);
        for (int n = 0; n < 5; ++n) need("corrupt_text0" + std::to_string(n) + x);
        for (int n = 0; n < 8; ++n) need("corrupt_icon_0" + std::to_string(n) + x);
    }
    for (const char* n : {"encyclopedia_back", "encyclopedia_header_text", "encyclopedia_text", "encyclopedia_right_text",
                          "encyclopedia_center_text", "encyclopedia_cost_text", "encyclopedia_icon", "zoomed_back", "zoomed_text",
                          "market_close", "bm_close", "corrupt_close", "hack_shell", "hack_title01", "hack_cost", "hack_hack",
                          "hack_close", "q1_fleet", "q2_fleet", "q3_fleet"})
        need(n);
    for (int k = 0; k < 7; ++k) need("hack_text_0" + std::to_string(k));
    HackPanel hk;
    CHECK(hk.init(table, atlas, fonts));
    size_t animated = 0;
    for (const CommandBarComponent& c : table.items()) animated += c.can_animate && !c.icon_alternate_texture.empty() ? 1u : 0u;
    std::printf("real panels: %zu Can_Animate components with frames\n", animated);
    CHECK(animated >= 60);
    std::printf("real panels: %zu components, %zu panel components missing\n", table.size(), missing);
    CHECK(missing == 0);
}
#else
void test_real_data() { std::printf("real screens: alamo_vfs not configured, skipped\n"); }
#endif

}  // namespace

// P2 completion: targeting controller (BeginAbilityTargeting / BeginReinforcement), radar jump, the galactic
// g_special_ability rows, and the S9 ability feed.
InputFrame mouse_frame(int32_t x, int32_t y, uint8_t down, uint8_t pressed, uint8_t released, double time) {
    InputFrame f;
    f.time = time;
    f.dt = 1.0f / 30.0f;
    f.width = 800;
    f.height = 600;
    f.mouse_x = x;
    f.mouse_y = y;
    f.buttons_down = down;
    f.buttons_pressed = pressed;
    f.buttons_released = released;
    return f;
}

void test_targeting() {
    CHECK(ability_target_kind(30) == TargetKind::Object && ability_target_kind(14) == TargetKind::Point &&
          ability_target_kind(26) == TargetKind::PointMode && ability_target_kind(1) == TargetKind::None &&
          ability_target_kind(17) == TargetKind::Point && ability_target_kind(6) == TargetKind::Object);
    TacticalCameraController cam;  // space defaults: the screen centre looks at the origin
    OrderContext ctx;
    ctx.mode = UiMode::Space;
    ctx.local_player = 0;
    ctx.view = make_view(cam.params(800.0f / 600.0f), 800, 600);
    tactical::TreeSet trees(2);
    tactical::TreeMembership m;
    m.owner = 1;
    CHECK(trees.add(static_cast<tactical::EntityHandle>(77), {{-20, -20, -20}, {20, 20, 20}}, 1u, m));
    ctx.trees = &trees;
    const uint8_t L = button_bit(Button::Left), R = button_bit(Button::Right);
    Bindings keys;
    auto click = [&](TargetingController& tc, InputState& in, int32_t x, int32_t y, uint8_t bit, double t, std::vector<UiOrder>& out) {
        in.update(mouse_frame(x, y, bit, bit, 0, t));
        const bool a = tc.update(in, keys, ctx, out);
        in.update(mouse_frame(x, y, 0, 0, bit, t + 0.05));
        const bool b = tc.update(in, keys, ctx, out);
        return a && b;
    };
    // Object ability (TRACTOR_BEAM 30): the click on the enemy at the origin -> command 40, queued from the HUD click.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 30;
        a.ids = {5, 6};
        a.queued = true;
        CHECK(tc.begin(a) && tc.active() && tc.target_kind() == TargetKind::Object);
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && out[0].queued && out[0].event == commands::ev::ability({5, 6}, 30, 77) && !tc.active() && tc.issued() == 1);
        HudAction other;
        other.kind = HudAction::Kind::RadarPing;
        CHECK(!tc.begin(other) && !tc.active());
    }
    // Point ability (14 Force Whirlwind): command 57 at the ground point; a click on empty sky keeps waiting.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 14;
        a.ids = {5};
        CHECK(tc.begin(a));
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && !out[0].queued && out[0].event.type() == commands::EventType::TacticalSpecialAbilityWithDummyTarget &&
              out[0].event.u("ability") == 14 && std::fabs(out[0].event.f("x")) < 1.0f && std::fabs(out[0].event.f("y")) < 1.0f &&
              out[0].event.ids("users") == std::vector<uint32_t>{5});
        // position via 43 (26 Distract): SetUnitAbilityMode on, with the point
        a.ability_type = 26;
        CHECK(tc.begin(a) && tc.target_kind() == TargetKind::PointMode);
        out.clear();
        CHECK(click(tc, in, 400, 300, L, 2.0, out));
        CHECK(out.size() == 1 && out[0].event.type() == commands::EventType::SetUnitAbilityMode && out[0].event.u("on") == 1 &&
              out[0].event.u("ability") == 26);
        // right click cancels
        CHECK(tc.begin(a));
        out.clear();
        CHECK(click(tc, in, 400, 300, R, 3.0, out));
        CHECK(out.empty() && !tc.active() && tc.cancelled() == 1);
        // not active: the world input is not taken
        in.update(mouse_frame(400, 300, L, L, 0, 4.0));
        CHECK(!tc.update(in, keys, ctx, out));
    }
    // Reinforcement drop: Reinforce at the ground point.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginReinforcement;
        a.unit = 4242;
        CHECK(tc.begin(a) && tc.mode() == TargetingController::Mode::Reinforcement);
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && out[0].event.type() == commands::EventType::Reinforce && out[0].event.u("unit") == 4242 && !tc.active());
    }
    // Host flow (apps/game battle_input): the router gives a running pick the presses (the HUD and the world see none of
    // them), the controller reads the RAW frame through its own InputState; the next press is the world's again.
    {
        TargetingController tc;
        InputRouter router;
        RouteHooks hooks;
        hooks.targeting_active = [&] { return tc.active(); };
        hooks.hud_hit = [](int32_t x, int32_t) { return x < 100 ? 7 : -1; };  // a HUD strip at the left edge
        RoutedInput routed;
        InputState raw_in, world_in;
        std::vector<UiOrder> out;
        bool world_click = false;
        auto frame = [&](int32_t x, int32_t y, uint8_t down, uint8_t pressed, uint8_t released, double t, bool esc = false) {
            InputFrame f = mouse_frame(x, y, down, pressed, released, t);
            if (esc) {
                f.keys_down.set(vk::kEscape);
                f.keys_pressed.set(vk::kEscape);
            }
            router.route(f, hooks, routed);
            raw_in.update(f);
            world_in.update(routed.world);
            world_click = world_click || world_in.button(Button::Left).click || world_in.button(Button::Right).click;
            return tc.update(raw_in, keys, ctx, out);
        };
        // no pick running: a click on the strip belongs to the HUD
        frame(50, 300, L, L, 0, 1.0);
        frame(50, 300, 0, 0, L, 1.05);
        CHECK(routed.hud.size() == 2 && routed.hud[1].kind == HudPointerEvent::Kind::Click && !tc.active());
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 30;
        a.value = -1;
        a.ids = {5};
        CHECK(tc.begin(a));
        // the pick runs: a click on the strip is the pick's (nothing under it: it keeps waiting), the HUD hears nothing
        CHECK(frame(50, 300, L, L, 0, 2.0) && routed.hud.empty() && routed.owner[0] == PointerTarget::Targeting);
        CHECK(frame(50, 300, 0, 0, L, 2.05) && routed.hud.empty() && tc.active() && out.empty());
        // the click on the enemy: command 40 through the order list, the world saw no click
        CHECK(frame(400, 300, L, L, 0, 3.0));
        CHECK(frame(400, 300, 0, 0, L, 3.05));
        CHECK(out.size() == 1 && out[0].event == commands::ev::ability({5}, 30, 77) && !tc.active() && !world_click);
        // the pick is over: the next press is the world's
        frame(400, 300, L, L, 0, 4.0);
        CHECK(routed.owner[0] == PointerTarget::World && !tc.update(raw_in, keys, ctx, out));
        frame(400, 300, 0, 0, L, 4.05);
        CHECK(world_click);
        // a right click cancels the pick without a world click; so does Esc
        out.clear();
        world_click = false;
        CHECK(tc.begin(a));
        frame(400, 300, R, R, 0, 5.0);
        frame(400, 300, 0, 0, R, 5.05);
        CHECK(!tc.active() && tc.cancelled() >= 1 && out.empty() && !world_click);
        CHECK(tc.begin(a));
        frame(400, 300, 0, 0, 0, 6.0, true);
        CHECK(!tc.active() && out.empty());
    }
    // The pick cursor.
    {
        using T = CursorContext::Targeting;
        CHECK(ability_pick_cursor(TargetKind::Object, 30, true, false) == T::AbilityEnemy &&
              ability_pick_cursor(TargetKind::Object, 30, true, true) == T::AbilityFriendly &&
              ability_pick_cursor(TargetKind::Point, 14, true, false) == T::AbilitySpace &&
              ability_pick_cursor(TargetKind::Point, 14, false, false) == T::AbilityTerrain &&
              ability_pick_cursor(TargetKind::Point, 70, false, false) == T::AbilityBlast &&
              ability_pick_cursor(TargetKind::PointMode, 5, true, false) == T::Barrage);
        CursorContext cc;
        cc.targeting = ability_pick_cursor(TargetKind::Object, 30, true, false);
        cc.target_valid = false;
        CHECK(choose_pointer(cc) == Pointer::AbilityEnemyInvalid);
        cc.target_valid = true;
        CHECK(choose_pointer(cc) == Pointer::AbilityEnemy);
    }
    // Radar camera jump.
    {
        TacticalCameraController c;
        HudAction a;
        a.kind = HudAction::Kind::RadarCameraJump;
        a.x = 1200;
        a.y = -800;
        CHECK(apply_radar_jump(a, c) && c.look_at().x == 1200.0f && c.look_at().y == -800.0f);
        a.kind = HudAction::Kind::RadarPing;
        CHECK(!apply_radar_jump(a, c));
    }

    // Galactic g_special_ability rows: one per provider entry beside the selected planet; a click -> the host action.
    {
        const char* xml = R"(<CommandBarComponents>
  <CommandBarComponent Name="g_special_ability"><Type>TextButton</Type><Blank_Texture_Name>i_row.tga</Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_sym_r.tga i_sym_e.tga</Icon_Alternate_Texture_Name><No_Shell>True</No_Shell><Base_Layer>0</Base_Layer>
    <Text_Offset>-18 0</Text_Offset><Offset>-48 -20</Offset><Right_Justified>True</Right_Justified></CommandBarComponent>
</CommandBarComponents>)";
        ComponentTable t;
        const data::XmlDocument d = data::XmlDocument::parse(std::string_view(xml), "s.xml");
        t.parse(d.root());
        TextureAtlas atlas;
        atlas.texture_width = atlas.texture_height = 64;
        atlas.add({"i_row.tga", 0, 0, 16, 16, true});
        atlas.add({"i_sym_r.tga", 16, 0, 12, 12, true});
        atlas.add({"i_sym_e.tga", 32, 0, 12, 12, true});
        auto raster = make_box_rasterizer();
        FontCache fonts(*raster);
        GalacticHud g;
        g.init(t, atlas, fonts);
        g.resize(1024, 768);
        GalacticCameraController gc;
        std::vector<GalacticPlanetInfo> planets(1);
        planets[0].planet = 300;
        planets[0].selected = true;
        GalacticHudFrame f;
        f.view = make_view(gc.params(4.0f / 3.0f), 1024, 768);
        f.planets = planets;
        f.providers.special_abilities = [](uint32_t planet, std::vector<GalacticSpecialAbility>& out) {
            if (planet != 300) return;
            out.push_back({901, U"Black market", "", "", true});
            out.push_back({902, U"Sabotage", "", "", true});
            out.push_back({903, U"Corrupt", "", "", false});
        };
        g.build(f);
        CHECK(g.stats().ability_rows == 3);
        const int32_t sc = t.find("g_special_ability");
        float sx = 0, sy = 0;
        CHECK(project(f.view, planets[0].position, sx, sy));
        const Vec2f at = g.canvas().pixel_to_canvas(sx, sy);
        auto row_px = [&](int i) { return g.canvas().canvas_to_pixel(at.x - 48.0f, at.y - 20.0f * static_cast<float>(i + 1)); };
        auto click_row = [&](int i, std::vector<HudAction>& acts) {
            const Vec2f p = row_px(i);
            const auto x = static_cast<int32_t>(p.x), y = static_cast<int32_t>(p.y);
            CHECK(g.hit_test(x, y) == sc);
            RoutedInput r;
            r.hover_component = sc;
            r.hud = {{HudPointerEvent::Kind::Press, Button::Left, sc, x, y}, {HudPointerEvent::Kind::Release, Button::Left, sc, x, y},
                     {HudPointerEvent::Kind::Click, Button::Left, sc, x, y}};
            std::vector<commands::EventData> cmds;
            g.update_input(r, false, false, 1.0, cmds, &acts);
            CHECK(cmds.empty());
        };
        std::vector<HudAction> acts;
        click_row(1, acts);
        CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::Button && acts[0].name == "g_special_ability" && acts[0].unit == 902 &&
              acts[0].value == 1 && acts[0].ids == std::vector<uint32_t>{300});
        acts.clear();
        click_row(2, acts);  // disabled row: no action
        CHECK(acts.empty());
        // frame + faction symbol per row (6 quads) and the texts; nothing without a selected planet
        CHECK(g.draw_list().items(0, HudList::Alpha).size() == 6 && g.draw_list().items(0, HudList::Text).size() > 3);
        planets[0].selected = false;
        g.build(f);
        CHECK(g.stats().ability_rows == 0);
    }

#ifdef ALAMO_UI_HAVE_ABILITIES
    // S9 ability feed: read-only AbilitySystem queries -> HudAbility rows.
    {
        using namespace alamo::tactical::abilities;
        tactical::combat::CombatData cd;
        AbilityData ad;
        tactical::combat::UnitDesc u;
        u.name = "Ship";
        u.tactical_health = 1000.0f;
        u.shield_points = 500.0f;  // DEFEND needs shields (Can_Activate gate 10)
        u.shield_refresh_rate = 10.0f;
        u.radius = 50.0f;
        AbilityObjectDesc od;
        UnitAbilityRecord defend;
        defend.type = kDefend;
        defend.combat.ability_type = kDefend;
        defend.expiration_seconds = 15.0f;
        defend.recharge_seconds = 20.0f;
        UnitAbilityRecord tractor;
        tractor.type = kTractorBeam;
        tractor.combat.ability_type = kTractorBeam;
        od.unit_abilities = {defend, tractor};
        cd.units.push_back(u);
        od.name = u.name;
        link_combat_abilities(od, cd, cd.units.back());
        ad.objects.push_back(od);
        tactical::combat::CombatSystem cs(cd, tactical::combat::CombatConfig{},
                                          std::vector<tactical::combat::PlayerInfo>{tactical::combat::PlayerInfo{}, tactical::combat::PlayerInfo{}});
        AbilitySystem as(cs, ad, AbilityConfig{});
        SimRng rng{77u};
        cs.bind_rng(&rng);
        tactical::combat::UnitSpawn s;
        s.id = 1;
        s.desc = 0;
        s.owner = 0;
        s.facing = {1, 0, 0};
        CHECK(as.add_unit(s, 0, 1, rng));
        tactical::TreeSet strees(2);
        tactical::TreeMembership sm;
        sm.owner = 0;
        CHECK(strees.add(static_cast<tactical::EntityHandle>(1), {{-50, -50, -50}, {50, 50, 50}}, 1u, sm));
        as.tick(1, strees, rng);  // first update (countdown deltas are 1 from here on, as in the S9 tests)
        SelectedGroup grp;
        grp.ids = {1};
        std::vector<HudAbility> out;
        abilities_from_s9(as, grp, 2, out);
        CHECK(out.size() == 2 && out[0].ability_type == kDefend && out[1].ability_type == kTractorBeam);
        CHECK(out.size() == 2 && !out[0].targeted && out[1].targeted && out[1].target_kind == static_cast<uint8_t>(TargetKind::Object) &&
              out[0].icon == "i_sa_defend_mode.tga" && out[0].recharge == 1.0f && !out[0].active && out[0].enabled);
        const tactical::combat::ObjectId ids[] = {1};
        CHECK(as.set_unit_ability_mode(ids, kDefend, true, 2) == 1);
        out.clear();
        abilities_from_s9(as, grp, 2, out);
        CHECK(!out.empty() && out[0].active && out[0].enabled);
        for (uint32_t fr = 2; fr < 32; ++fr) as.tick(fr, strees, rng);  // 30 active frames
        CHECK(as.set_unit_ability_mode(ids, kDefend, false, 32) == 1);  // early off: recharging
        out.clear();
        auto provider = s9_ability_provider(as, [] { return 32u; });
        provider(grp, out);
        CHECK(!out.empty() && !out[0].active && out[0].recharge < 1.0f);
        // t42: a record's Alternate_Icon_Name replaces the type's art on the button (the parse: test_abilities).
        CHECK(ad.objects[0].unit_abilities[0].type == kDefend);
        ad.objects[0].unit_abilities[0].alternate_icon_name = "i_sa_strike.tga";
        out.clear();
        abilities_from_s9(as, grp, 32, out);
        CHECK(!out.empty() && out[0].icon == "i_sa_strike.tga" && out.size() == 2 && out[1].icon == "i_sa_tractor_beam.tga");
        // QA D5: the i_sa_* art of MT_CommandBar, by mapping or by type name.
        CHECK(ability_icon(0).empty() && ability_icon(kDefend) == "i_sa_defend_mode.tga" &&
              ability_icon(kTractorBeam) == "i_sa_tractor_beam.tga" && ability_icon(kLure) == "i_sa_lure.tga" &&
              ability_icon(kStun) == "i_sa_stun.tga");
        std::printf("targeting: S9 feed %zu abilities for the test ship\n", out.size());
    }
#endif
}

int main() {
    test_screens();
    test_advanced_video();
    test_video_vsync_mode();
    test_video_port_text();
    test_resolution_confirm();
    test_options_polish();
    test_advanced_video_port();
    test_lobby();
    test_panels();
    test_encyclopedia_fade_anchor();
    test_camera_hotkeys();
    test_info_screens();
    test_cursor();
    test_anim_hack_drag();
    test_controls();
    test_targeting();
    test_real_data();
    if (g_fail) {
        std::printf("alamo_ui_screens_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_screens_tests: all passed\n");
    return 0;
}
