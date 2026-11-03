// Shared fixtures and helpers of test_gui_screens*.cpp (split from test_gui_screens.cpp, PAF t08). Test bodies live in the part files.
#pragma once

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

inline int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace test_gui_screens_t {

// Trimmed copies of the shipped dialogs (same ids / rectangles).
inline const char* kRc = R"RC(
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

inline const char* kSkinXml = R"(<GUIDialogs><Textures File="MT_CommandBar"><Default>
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

inline GuiEvent event(GuiEvent::Kind k, std::string_view dialog, std::string_view control, int32_t value = 0) {
    GuiEvent e;
    e.kind = k;
    e.dialog = std::string(dialog);
    e.control = std::string(control);
    e.value = value;
    return e;
}

// ---- HUD panels ------------------------------------------------------------------------------------------------
inline const char* kPanelXml = R"(<CommandBarComponents>
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

inline const char* kFleetXml = R"(<CommandBarComponents>
  <CommandBarComponent Name="q1_fleet"><Type>Button</Type><Icon_Alternate_Texture_Name>i_big.tga i_big.tga</Icon_Alternate_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q2_fleet"><Type>Button</Type><Icon_Alternate_Texture_Name>i_big.tga i_big.tga</Icon_Alternate_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q1_0001"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q1_0002"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
  <CommandBarComponent Name="q2_0001"><Type>TextButton</Type><Blank_Texture_Name>i_frame.tga</Blank_Texture_Name><Drag_And_Drop>True</Drag_And_Drop></CommandBarComponent>
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

// ---- [port: Gabriel] camera and controls: pause, FOV, free rotation, W A S D, queued orders ------------------------
inline InputFrame key_frame(std::initializer_list<uint16_t> down, std::initializer_list<uint16_t> pressed, double time) {
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

// Tests defined in the part files.
void test_screens();
void test_advanced_video();
void test_resolution_confirm();
void test_video_port_text();
void test_video_vsync_mode();
void test_options_polish();
void test_advanced_video_port();
void test_lobby();
void test_encyclopedia_fade_anchor();
void test_panels();
void test_info_screens();
void test_cursor();
void test_anim_hack_drag();
void test_controls();
void test_camera_hotkeys();

}  // namespace test_gui_screens_t
