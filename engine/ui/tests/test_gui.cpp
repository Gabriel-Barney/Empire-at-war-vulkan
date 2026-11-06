// alamo_ui_gui_tests (P4, CPU): GUIDialogs.rc / Resource.h / GUIDialogs.xml parsing, dialog placement (dialog unit ->
// canvas), controls (buttons, check / radio groups, sliders, lists, combos, edits), focus and keyboard navigation,
// modal routing through InputRouter, the menu flow (main menu -> single player -> campaign selection, pause menu ->
// save / quit, audio options, keyboard capture), load screens (Begin hidden -> shown + focused + GUI_Text_Hint_SFX,
// multiplayer bars), the galactic HUD (labels, zoom alpha, build pads -> Production, queue cancel, fleet pads,
// objectives) and the Lua GUI ops. With ALAMO_FOC_DATA: the shipped files through the VFS and the real campaign list.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/gui_script.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/load_screen.hpp"
#include "alamo/ui/picking.hpp"
#ifdef ALAMO_UI_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif
#ifdef ALAMO_UI_HAVE_LUA
#include "alamo/lua/presentation.hpp"
#include "alamo/ui/gui_script_lua.hpp"
#endif

using namespace alamo;
using namespace alamo::ui;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

// A trimmed resource script in the shipped format (same ids / captions / rectangles as the vanilla dialogs it names,
// plus a test dialog), with the APSTUDIO blocks the parser must skip.
const char* kRc = R"RC(// Microsoft Visual C++ generated resource script.
//
#include "resource.h"
#define APSTUDIO_READONLY_SYMBOLS
#include "afxres.h"
#if !defined(AFX_RESOURCE_DLL) || defined(AFX_TARG_ENU)
LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US
#pragma code_page(1252)
#endif //_WIN32

IDD_MAIN_MENU_DIALOG DIALOGEX 0, 0, 512, 1024
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION |
    WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial Black", 700, 0, 0x0
BEGIN
    PUSHBUTTON      "",IDC_MENU_CREDITS,295,381,160,160
    PUSHBUTTON      "TEXT_BUTTON_SINGLE_PLAYER_GAMES",
                    IDC_MAIN_MENU_SINGLE_PLAYER_GAMES,265,562,236,22
    PUSHBUTTON      "TEXT_BUTTON_MULTIPLAYER",IDC_MAIN_MENU_MULTIPLAYER,265,
                    598,236,23
    PUSHBUTTON      "TEXT_BUTTON_LOAD_GAME",IDC_LOAD_GAME_MAIN,265,634,236,
                    21
    PUSHBUTTON      "TEXT_BUTTON_EXIT_GAME",IDC_MAIN_MENU_EXIT2,265,706,236,
                    22
    PUSHBUTTON      "TEXT_BUTTON_OPTIONS",IDC_OPTIONS,265,670,236,22
    CTEXT           "TEXT_MAIN_MENU",IDC_STATIC_OPTIONS,257,486,254,16
    PUSHBUTTON      "TEXT_BUTTON_TUTORIAL",IDC_MAIN_MENU_TUTORIAL,265,526,
                    236,22
    PUSHBUTTON      "TEXT_BUTTON_PLAY_DEMO",IDC_MAIN_MENU_PLAY_DEMO,265,562,
                    236,22
    PUSHBUTTON      "",IDC_MENU_PETRO_LOGO,-374,642,118,118
END

IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG DIALOGEX 0, 0, 512, 1024
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial Black", 700, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_NEW_CAMPAIGN",IDC_NEW_CAMPAIGN_BUTTON,265,526,236,22
    PUSHBUTTON      "TEXT_BUTTON_CUSTOM_CAMPAIGN",IDC_CUSTOM_CAMPAIGN_BUTTON,265,562,236,23
    PUSHBUTTON      "TEXT_BUTTON_BACK",IDC_BACK_BUTTON_MAIN,265,634,237,22
    PUSHBUTTON      "TEXT_BUTTON_SKIRMISH",IDC_SKIRMISH_GAMES_BUTTON,265,598,236,23
    PUSHBUTTON      "TEXT_BUTTON_DEBUG_LOAD_SKIRMISH_MAP",
                    IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON,284,723,196,22
END

IDD_CAMPAIGN_SELECTION_DIALOG DIALOGEX 0, 0, 620, 441
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_CANCEL_BUTTON,19,398,190,24
    PUSHBUTTON      "TEXT_BUTTON_PLAY_FACTION_B",IDC_PLAY_FACTION_B_BUTTON,405,207,190,24
    LISTBOX         IDC_CAMPAIGNS_LIST,18,68,380,327,LBS_SORT |
                    LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LTEXT           "TEXT_SELECT_CAMPAIGN_TITLE",IDC_STATIC,20,42,566,18
    PUSHBUTTON      "TEXT_BUTTON_PLAY_FACTION_A",IDC_PLAY_FACTION_A_BUTTON,405,173,190,24
    CONTROL         "",IDC_CAMPAIGN_DIFFICULTY_SLIDER,"msctls_trackbar32",
                    TBS_BOTH | TBS_NOTICKS | WS_TABSTOP,407,111,187,17
    LTEXT           "",IDC_STATIC_DIFFICULTY_DESCRIPTION,408,89,200,16
    LTEXT           "TEXT_CAMPAIGN_DIFFICULTY",IDC_STATIC_DIFFICULTY_NAME,506,68,99,18
END

IDD_GAME_OPTIONS_DIALOG DIALOGEX 0, 0, 321, 328
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial Black", 700, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_LOAD_GAME",IDC_LOAD_GAME,41,103,239,24
    PUSHBUTTON      "TEXT_BUTTON_QUIT_GAME",IDC_GAME_OPTIONS_QUIT,41,232,239,24
    PUSHBUTTON      "TEXT_BUTTON_SAVE_GAME",IDC_SAVE_GAME,41,60,239,24
    LTEXT           "TEXT_GAME_OPTIONS_TITLE",IDC_STATIC_MEDIUM,20,11,289,26
    PUSHBUTTON      "TEXT_BUTTON_RESUME_GAME",IDC_GAME_OPTIONS_RESUME,41,275,239,24
    PUSHBUTTON      "TEXT_BUTTON_OPTIONS",IDC_INGAME_OPTIONS,41,146,239,24
END

IDD_SAVE_GAME_DIALOG DIALOGEX 0, 0, 640, 520
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_SAVE_CANCEL_BUTTON,17,477,176,24
    PUSHBUTTON      "TEXT_BUTTON_SAVE",IDC_SAVE_GAME_BUTTON,416,477,176,24
    EDITTEXT        IDC_SAVE_GAME_NAME_EDIT,115,443,388,18,ES_AUTOHSCROLL
    LISTBOX         IDC_SAVE_GAME_LIST,16,68,618,372,LBS_SORT |
                    LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    PUSHBUTTON      "TEXT_BUTTON_DELETE",IDC_DELETE_GAME_BUTTON,217,477,176,24
END

IDD_DIALOG_BOX_QUIT_CONFIRM DIALOGEX 0, 0, 337, 144
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    CTEXT           "TEXT_CAPTION",IDC_CAPTION,22,20,286,15
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_LEFT,22,95,128,24
    PUSHBUTTON      "TEXT_BUTTON",IDC_DIALOG_BOX_BUTTON_RIGHT,181,95,128,24
    LTEXT           "USER STRING 1 HERE",IDC_USER_TEXT_STRING_1,17,45,296,8
END

IDD_AUDIO_OPTIONS_DIALOG DIALOGEX 0, 0, 551, 372
STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 6, "Arial Black", 700, 0, 0x0
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_ACCEPT",IDC_OK,388,330,140,24
    PUSHBUTTON      "TEXT_BUTTON_CANCEL",IDC_CANCEL,14,330,140,24
    CONTROL         "",IDC_SLIDER_MASTER_VOLUME,"msctls_trackbar32",TBS_BOTH |
                    TBS_NOTICKS,287,125,242,15
    CONTROL         "",IDC_SLIDER_MUSIC_VOLUME,"msctls_trackbar32",TBS_BOTH |
                    TBS_NOTICKS,287,236,242,15
END

IDD_KEYBOARD_OPTIONS_DIALOG DIALOGEX 0, 0, 880, 542
STYLE DS_SETFONT | DS_MODALFRAME | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "MS Shell Dlg", 400, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_BACK",IDC_OK,15,502,173,26
    PUSHBUTTON      "TEXT_BUTTON_ACCEPT",IDC_APPLY,685,502,173,26
    EDITTEXT        IDC_EDIT_STOP,303,360,108,15,ES_CENTER | ES_AUTOHSCROLL
    EDITTEXT        IDC_EDIT_GROUP_1_SELECT,303,60,108,15,ES_CENTER | ES_AUTOHSCROLL
END

IDD_SKIRMISH_BATTLE_LOAD_DIALOG DIALOGEX 0, 0, 975, 718
STYLE DS_SETFONT | DS_MODALFRAME | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Dialog"
FONT 8, "MS Shell Dlg", 400, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "TEXT_BUTTON_BEGIN",IDC_BEGIN_BUTTON,575,665,168,33
    LISTBOX         IDC_LIST_REFERENCE,51,184,391,452,LBS_SORT |
                    LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LTEXT           "current planet name",IDC_STATIC_PLANET_NAME,91,80,623,18
    CONTROL         "",IDC_PLANET_ICON,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,
                    464,166,435,435
    CONTROL         "",IDC_PROGRESS_SOLO,"msctls_progress32",WS_BORDER,452,
                    617,465,21
    CONTROL         "",IDC_PROGRESS0,"msctls_progress32",WS_BORDER,661,597,224,14
    CONTROL         "",IDC_PROGRESS1,"msctls_progress32",WS_BORDER,661,580,224,14
    RTEXT           "Name 0",IDC_PLAYER_NAME_0,401,595,247,18
    RTEXT           "Name 1",IDC_PLAYER_NAME_1,401,578,247,18
    LTEXT           "TEXT_SAVELOAD_LOADING_MSG",IDC_STATIC_LOADING,457,616,445,14
END

IDD_TEST_CONTROLS DIALOGEX 0, 0, 400, 300
STYLE DS_SETFONT | WS_POPUP
CAPTION "Quote ""test"""
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    DEFPUSHBUTTON   "OK",IDC_TEST_OK,10,260,80,24
    PUSHBUTTON      "Hidden",IDC_TEST_HIDDEN,100,260,80,24,NOT WS_VISIBLE
    PUSHBUTTON      "Off",IDC_TEST_OFF,190,260,80,24,WS_DISABLED | NOT WS_TABSTOP
    CONTROL         "Check",IDC_TEST_CHECK,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,10,10,100,12
    CONTROL         "A",IDC_TEST_RA,"Button",BS_AUTORADIOBUTTON | WS_GROUP,10,30,80,12
    CONTROL         "B",IDC_TEST_RB,"Button",BS_AUTORADIOBUTTON,10,45,80,12
    CONTROL         "Tab",IDC_TEST_RC,"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,10,60,80,16
    CONTROL         "",IDC_TEST_SLIDER,"msctls_trackbar32",TBS_BOTH | WS_TABSTOP,120,10,200,15
    LISTBOX         IDC_TEST_LIST,120,40,200,60,LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP
    COMBOBOX        IDC_TEST_COMBO,120,110,150,80,CBS_DROPDOWN | WS_TABSTOP
    EDITTEXT        IDC_TEST_EDIT,120,200,150,16,ES_AUTOHSCROLL
    LTEXT           "Label",IDC_STATIC,10,90,100,12
    RTEXT           "Right",IDC_STATIC,10,105,100,12
    CONTROL         "",IDC_TEST_PROGRESS,"msctls_progress32",WS_BORDER,10,130,100,10
    CONTROL         "Custom2",IDC_TEST_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,330,10,60,60
    GROUPBOX        "Static",IDC_STATIC,5,5,115,85
END

#ifdef APSTUDIO_INVOKED
GUIDELINES DESIGNINFO
BEGIN
    IDD_MAIN_MENU_DIALOG, DIALOG
    BEGIN
        LEFTMARGIN, 7
        BOTTOMMARGIN, 528
    END
END
#endif    // APSTUDIO_INVOKED

1 TEXTINCLUDE
BEGIN
    "resource.h\0"
END
)RC";

const char* kResourceH = R"(//{{NO_DEPENDENCIES}}
#define IDD_MAIN_MENU_DIALOG            5
#define IDD_LAN_LOBBY                   103
#define IDD_LAN_LOBBY_DIALOG            103
#define IDC_MENU_PETRO_LOGO             1988
#define IDC_HEX                         0x7f0
#ifdef APSTUDIO_INVOKED
#define _APS_NEXT_RESOURCE_VALUE        217
#endif
)";

const char* kSkinXml = R"(<?xml version="1.0" encoding="utf-8"?>
<GUIDialogs>
  <Textures File="MT_CommandBar" Compressed_File="MT_CommandBarCompressed">
    <Default>
      <Frame_Top_Left>i_dialogue_00.tga</Frame_Top_Left>
      <Frame_Background>i_dialogue_frame_middle_thick.tga</Frame_Background>
      <Button_Left>i_dialogue_button_large_left.tga</Button_Left>
      <Button_Middle>i_dialogue_button_large_middle.tga</Button_Middle>
      <Button_Right>i_dialogue_button_large_right.tga</Button_Right>
      <Button_Middle_Mouse_Over>i_dialogue_button_large_middle_mouse_over.tga</Button_Middle_Mouse_Over>
      <Check_On>i_dialogue_check_on.tga</Check_On>
    </Default>
    <!-- override -->
    <IDD_MAIN_MENU_DIALOG>
      <Frame_Top_Left>none</Frame_Top_Left>
      <Frame_Background>MenuBack_overlay_panel.tga</Frame_Background>
    </IDD_MAIN_MENU_DIALOG>
    <IDC_MENU_CREDITS>
      <Button_Right>i_button_credits.tga</Button_Right>
    </IDC_MENU_CREDITS>
  </Textures>
  <Fonts>
    <Default>
      <Global_Default><Name>EmpireAtWar-Medium</Name><Size>7</Size><Top_Color>195,171,2,255</Top_Color>
        <Bottom_Color>195,171,2,255</Bottom_Color><Emboss>No</Emboss><Outline>No</Outline></Global_Default>
      <Push_Button><Name>EmpireAtWar-Bold</Name><Character_Padding>0</Character_Padding><Stretch_Factor>1</Stretch_Factor>
        <Size>8</Size><Bottom_Color>253,248,212,255</Bottom_Color><Top_Color>253,248,212,255</Top_Color><Emboss>Yes</Emboss>
        <Outline>No</Outline></Push_Button>
      <L_Text><Name>EmpireAtWar-Medium</Name><Size>7</Size><Stretch_Factor>1.3</Stretch_Factor>
        <Top_Color>253,248,212,255</Top_Color><Bottom_Color>200,100,50,255</Bottom_Color></L_Text>
    </Default>
    <IDC_HINT_LIST><Name>Arial Unicode MS</Name><Size>7</Size><Top_Color>255,255,255,255</Top_Color>
      <Bottom_Color>255,255,255,255</Bottom_Color><Emboss>No</Emboss><Outline>No</Outline></IDC_HINT_LIST>
    <IDD_SKIRMISH_BATTLE_LOAD_DIALOG><L_Text><Name>EmpireAtWar-Medium</Name><Size>8</Size>
      <Top_Color>195,171,2,255</Top_Color><Bottom_Color>195,171,2,255</Bottom_Color></L_Text></IDD_SKIRMISH_BATTLE_LOAD_DIALOG>
  </Fonts>
  <Tooltips><IDC_TEST_CHECK><TextID>TEXT_TIP</TextID></IDC_TEST_CHECK></Tooltips>
</GUIDialogs>
)";

TextureAtlas gui_atlas() {
    TextureAtlas a;
    a.texture_width = a.texture_height = 256;
    a.add({"i_dialogue_00.tga", 0, 0, 16, 16, true});
    a.add({"i_dialogue_frame_middle_thick.tga", 16, 0, 16, 16, true});
    a.add({"i_dialogue_button_large_left.tga", 32, 0, 8, 32, true});
    a.add({"i_dialogue_button_large_middle.tga", 40, 0, 8, 32, true});
    a.add({"i_dialogue_button_large_right.tga", 48, 0, 8, 32, true});
    a.add({"i_dialogue_button_large_middle_mouse_over.tga", 56, 0, 8, 32, true});
    a.add({"i_dialogue_check_on.tga", 64, 0, 12, 12, true});
    return a;
}

struct Env {
    DialogScript script;
    ResourceIds ids;
    GuiSkin skin;
    TextureAtlas atlas = gui_atlas();
    TextDatabase text;
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    GuiManager gui;
    InputRouter router;
    std::vector<std::string> sfx;
    double now = 10.0;
    uint8_t down = 0;
    Env() {
        std::string err;
        script.parse(kRc, &err);
        ids.parse(kResourceH);
        const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kSkinXml), "GUIDialogs.xml");
        skin.parse(doc.root());
        text.add("TEXT_BUTTON_BEGIN", u"Begin");
        text.add("TEXT_DIFFICULTY_NORMAL_NAME", u"Normal");
        text.add("TEXT_BUTTON_YES", u"Yes");
        gui.init(script, skin, atlas, fonts, &text);
        gui.resize(1024, 768);
        gui.set_sfx_hook([this](std::string_view e, int32_t) { sfx.emplace_back(e); });
    }
    Vec2f px_of(const GuiDialog& d, std::string_view id) const {
        const GuiRect r = d.control_rect(static_cast<size_t>(d.find(id)));
        return gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y) - 2.0f);
    }
    // One frame: mouse at (x, y), left button `left` (held after the frame), keys pressed this frame.
    std::vector<GuiEvent> frame(Vec2f px, bool left, std::initializer_list<uint16_t> keys = {}, bool shift = false,
                                std::u32string_view typed = {}, RoutedInput* out = nullptr) {
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
        if (shift) f.keys_down.set(vk::kShift);
        RouteHooks hooks;
        gui.install(hooks);
        RoutedInput ri;
        router.route(f, hooks, ri);
        std::vector<GuiEvent> ev;
        gui.update(f, ri, now, ev, typed);
        if (out) *out = ri;
        now += 0.05;
        last = f;
        return ev;
    }
    std::vector<GuiEvent> click(const GuiDialog& d, std::string_view id) {
        const Vec2f p = px_of(d, id);
        std::vector<GuiEvent> ev = frame(p, false);
        std::vector<GuiEvent> a = frame(p, true);
        std::vector<GuiEvent> b = frame(p, false);
        ev.insert(ev.end(), a.begin(), a.end());
        ev.insert(ev.end(), b.begin(), b.end());
        return ev;
    }
    InputFrame last;
};

bool has_event(const std::vector<GuiEvent>& ev, GuiEvent::Kind k, std::string_view control, int32_t value = -999) {
    for (const GuiEvent& e : ev)
        if (e.kind == k && (control.empty() || e.control == control) && (value == -999 || e.value == value)) return true;
    return false;
}

void test_parse() {
    Env env;
    const DialogScript& s = env.script;
    CHECK(s.dialogs().size() == 10);
    CHECK(s.warnings.empty());
    for (const std::string& w : s.warnings) std::printf("  warning: %s\n", w.c_str());
    const RcDialog* mm = s.find("IDD_MAIN_MENU_DIALOG");
    CHECK(mm && mm->w == 512 && mm->h == 1024 && mm->centered() && mm->font_size == 8 && mm->font_face == "Arial Black");
    CHECK(mm && mm->controls.size() == 10);
    if (mm) {
        const RcControl& sp = mm->controls[1];  // continuation lines
        CHECK(sp.id == "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES" && sp.caption == "TEXT_BUTTON_SINGLE_PLAYER_GAMES" && sp.x == 265 &&
              sp.y == 562 && sp.w == 236 && sp.h == 22 && sp.gadget == GuiGadgetType::PushButton && sp.tabstop);
        const RcControl& logo = mm->controls[9];
        CHECK(logo.x == -374 && logo.id == "IDC_MENU_PETRO_LOGO");
        CHECK(mm->controls[6].gadget == GuiGadgetType::Text && mm->controls[6].align == TextAlign::Center && !mm->controls[6].tabstop);
    }
    const RcDialog* t = s.find("idd_test_controls");  // case-insensitive lookup
    CHECK(t && t->caption == "Quote \"test\"");
    if (t) {
        auto c = [&](std::string_view id) { return t->controls[static_cast<size_t>(t->find(id))]; };
        CHECK(c("IDC_TEST_OK").default_button && c("IDC_TEST_OK").tabstop);
        CHECK(!c("IDC_TEST_HIDDEN").visible && c("IDC_TEST_HIDDEN").has_style("!WS_VISIBLE"));
        CHECK(c("IDC_TEST_OFF").disabled && !c("IDC_TEST_OFF").tabstop);
        CHECK(c("IDC_TEST_CHECK").gadget == GuiGadgetType::CheckBox && c("IDC_TEST_CHECK").class_name == "Button");
        CHECK(c("IDC_TEST_RA").gadget == GuiGadgetType::RadioButton && c("IDC_TEST_RA").group);
        CHECK(c("IDC_TEST_RC").pushlike);
        CHECK(c("IDC_TEST_SLIDER").gadget == GuiGadgetType::TrackbarSlider && c("IDC_TEST_SLIDER").tabstop);
        CHECK(c("IDC_TEST_LIST").gadget == GuiGadgetType::ListBox && c("IDC_TEST_COMBO").gadget == GuiGadgetType::ComboBox);
        CHECK(c("IDC_TEST_EDIT").gadget == GuiGadgetType::EditBox && c("IDC_TEST_EDIT").tabstop);
        CHECK(c("IDC_TEST_PROGRESS").gadget == GuiGadgetType::ProgressBar);
        CHECK(c("IDC_TEST_IMAGE").gadget == GuiGadgetType::CustomImage && !c("IDC_TEST_IMAGE").tabstop);
        CHECK(t->controls.back().gadget == GuiGadgetType::Frame);
    }
    CHECK(env.ids.size() == 6 && env.ids.id("IDC_HEX") == 0x7f0 && env.ids.id("IDD_LAN_LOBBY_DIALOG") == 103 &&
          env.ids.name(103) == "IDD_LAN_LOBBY" && !env.ids.id("IDC_NOPE").has_value());
    // Skin: precedence control > dialog > Default, "none" = no texture.
    const GuiSkin& k = env.skin;
    CHECK(k.atlas_file == "MT_CommandBar" && k.compressed_atlas_file == "MT_CommandBarCompressed");
    CHECK(k.texture("IDD_SAVE_GAME_DIALOG", "", "Frame_Top_Left") == "i_dialogue_00.tga");
    CHECK(k.texture("IDD_MAIN_MENU_DIALOG", "", "Frame_Top_Left").empty());
    CHECK(k.texture("IDD_MAIN_MENU_DIALOG", "IDC_X", "frame_background") == "MenuBack_overlay_panel.tga");
    CHECK(k.texture("IDD_MAIN_MENU_DIALOG", "IDC_MENU_CREDITS", "Button_Right") == "i_button_credits.tga");
    CHECK(k.texture("IDD_MAIN_MENU_DIALOG", "IDC_MENU_CREDITS", "Button_Left") == "i_dialogue_button_large_left.tga");
    CHECK(k.texture("", "", "Unknown_Key").empty());
    const GuiFontSpec& pb = k.font("IDD_X", "IDC_Y", GuiGadgetType::PushButton, TextAlign::Center);
    CHECK(pb.face == "EmpireAtWar-Bold" && pb.size == 8.0f && pb.emboss && (pb.top == Rgba8{253, 248, 212, 255}));
    const GuiFontSpec& lt = k.font("IDD_X", "IDC_Y", GuiGadgetType::Text, TextAlign::Left);
    CHECK(lt.stretch == 1.3f && (lt.bottom == Rgba8{200, 100, 50, 255}) && !lt.emboss);
    CHECK(k.font("IDD_SKIRMISH_BATTLE_LOAD_DIALOG", "IDC_STATIC", GuiGadgetType::Text, TextAlign::Left).size == 8.0f);
    CHECK(k.font("IDD_X", "IDC_HINT_LIST", GuiGadgetType::ListBox, TextAlign::Left).face == "Arial Unicode MS");
    CHECK(k.font("IDD_X", "", GuiGadgetType::Text, TextAlign::Center).face == "EmpireAtWar-Medium");  // C_Text -> global
    CHECK(k.tooltip("idc_test_check") == "TEXT_TIP" && k.tooltip_count() == 1);
}

void test_placement() {
    Env env;
    GuiDialog* mm = env.gui.open("IDD_MAIN_MENU_DIALOG");
    CHECK(mm != nullptr);
    if (!mm) return;
    // Column dialog (taller than the canvas): top-right corner; 1 dialog unit = 1 canvas unit = 1 pixel at 1024 x 768.
    CHECK(mm->origin() == (Vec2f{0.0f, 384.0f}));
    const Vec2f sp = env.gui.canvas().canvas_to_pixel(mm->control_rect(static_cast<size_t>(mm->find("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"))).min.x,
                                                      mm->control_rect(static_cast<size_t>(mm->find("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"))).max.y);
    CHECK(std::fabs(sp.x - (512.0f + 265.0f)) < 0.01f && std::fabs(sp.y - 562.0f) < 0.01f);
    const GuiRect logo = mm->control_rect(static_cast<size_t>(mm->find("IDC_MENU_PETRO_LOGO")));
    CHECK(std::fabs(logo.min.x - (-374.0f)) < 0.01f);  // left of the column
    GuiDialog* qc = env.gui.open("IDD_DIALOG_BOX_QUIT_CONFIRM");
    CHECK(qc && qc->origin() == (Vec2f{-169.0f, 72.0f}));  // centred (337 x 144)
    // Widescreen: canvas widens to 1365.3 units, the column moves to the screen's right edge (base 20.jpg), units stay
    // 768 tall; the Petroglyph hotspot follows the stretched backdrop (GALAXY_MAP_PRESENTATION §5: 4:3 x x 1365.3 / 1024).
    env.gui.resize(1920, 1080);
    CHECK(std::fabs(mm->origin().x - (0.5f * env.gui.canvas().width - 512.0f)) < 0.01f && mm->origin().y == 384.0f);
    {
        HudDrawList dl;
        env.gui.build(dl, 0.0);
        const GuiRect lw = mm->control_rect(static_cast<size_t>(mm->find("IDC_MENU_PETRO_LOGO")));
        const float sx = env.gui.canvas().width / 1024.0f;
        CHECK(std::fabs(0.5f * (lw.min.x + lw.max.x) - (-374.0f + 59.0f) * sx) < 0.01f && std::fabs(lw.width() - 118.0f * sx) < 0.01f &&
              std::fabs(lw.height() - 118.0f) < 0.01f);
        const Vec2f lp = env.gui.canvas().canvas_to_pixel(lw.min.x, lw.max.y);
        std::printf("placement: petro hotspot left edge %.1f px at 1920 x 1080\n", lp.x);
    }
    env.gui.resize(1280, 1024);  // taller than 4:3: canvas 1024 x 819.2, top edge follows
    CHECK(mm->origin().x == 0.0f && std::fabs(mm->origin().y - 409.6f) < 0.01f);
    // Playtest 3 t13: the Petroglyph button's art lands on the logo painted into MenuBack_overlay at every aspect. Measured
    // from the shipped art (Patch2.meg): the logo's opaque pixels span x 145..262, y 668..748 of the 1024 x 768 overlay;
    // i_button_petro (118 x 118, drawn grown to 128 x 128 by three_piece) has its content at x 7..115, y 24..98. The overlay
    // is one quad over the whole canvas, so the content must map through the same stretch.
    {
        struct Size {
            uint32_t w, h;
        };
        for (const Size sz : {Size{1024, 768}, Size{1280, 720}, Size{1920, 1080}, Size{2560, 1440}, Size{3440, 1440}, Size{1280, 1024}, Size{1920, 1200}}) {
            env.gui.resize(sz.w, sz.h);
            HudDrawList dl;
            env.gui.build(dl, 0.0);
            const GuiRect r = mm->control_rect(static_cast<size_t>(mm->find("IDC_MENU_PETRO_LOGO")));
            const float grow = 128.0f / 118.0f;  // the quad three_piece draws: top-left anchored, grown to the next power of two
            const float qw = r.width() * grow, qh = r.height() * grow;
            const GuiRect content{{r.min.x + qw * 7.0f / 118.0f, r.max.y - qh * 99.0f / 118.0f},  // art edges x 7 .. 116, y 24 .. 99 of 118
                                  {r.min.x + qw * 116.0f / 118.0f, r.max.y - qh * 24.0f / 118.0f}};
            const float cw = env.gui.canvas().width, ch = env.gui.canvas().height;
            const float psx = cw / 1024.0f, psy = ch / 768.0f;
            const GuiRect painted{{-0.5f * cw + 145.0f * psx, 0.5f * ch - 749.0f * psy}, {-0.5f * cw + 263.0f * psx, 0.5f * ch - 668.0f * psy}};
            const float tol = 1.6f * std::max(psx, psy);  // ~1.5 base pixels (the art is scaled by 128 / 118, not exactly 1)
            const bool ok = std::fabs(content.min.x - painted.min.x) < tol && std::fabs(content.max.x - painted.max.x) < tol &&
                            std::fabs(content.min.y - painted.min.y) < tol && std::fabs(content.max.y - painted.max.y) < tol;
            if (!ok)
                std::printf("petro logo off at %u x %u: content %.1f..%.1f x %.1f..%.1f vs painted %.1f..%.1f x %.1f..%.1f\n", sz.w, sz.h,
                            content.min.x, content.max.x, content.min.y, content.max.y, painted.min.x, painted.max.x, painted.min.y,
                            painted.max.y);
            CHECK(ok);
        }
    }
    env.gui.resize(1920, 1080);
    const float px_per_unit = 1.0f / env.gui.canvas().units_per_pixel_y;
    CHECK(std::fabs(px_per_unit - 1080.0f / 768.0f) < 1e-4f);
    // Stack order, bring-to-top, close.
    CHECK(env.gui.open_count() == 2 && env.gui.top() == qc);
    env.gui.open("IDD_MAIN_MENU_DIALOG");
    CHECK(env.gui.top() == mm && env.gui.open_names().front() == "IDD_DIALOG_BOX_QUIT_CONFIRM");
    env.gui.close("IDD_MAIN_MENU_DIALOG");
    CHECK(env.gui.open_count() == 1 && env.gui.top() == qc);
    CHECK(env.gui.open("IDD_NOPE") == nullptr);
    // Captions: text key resolved, else as written.
    GuiDialog* lo = env.gui.open("IDD_SKIRMISH_BATTLE_LOAD_DIALOG");
    CHECK(lo && lo->get("IDC_BEGIN_BUTTON")->text == U"Begin" && lo->get("IDC_PLAYER_NAME_0")->text == U"Name 0");
}

void test_routing_and_controls() {
    Env env;
    GuiDialog* d = env.gui.open("IDD_TEST_CONTROLS");
    CHECK(d != nullptr);
    if (!d) return;
    // Modal: a press anywhere goes to the dialog; the world sees nothing; keys go to the dialog.
    RoutedInput ri;
    env.frame({5, 5}, true, {}, false, {}, &ri);
    CHECK(ri.owner[0] == PointerTarget::Dialog && ri.world.buttons_pressed == 0 && !ri.keys_to_world);
    env.frame({5, 5}, false);
    // Non-modal: only presses inside it.
    env.gui.open("IDD_TEST_CONTROLS", GuiOpenOptions{false, false});
    env.frame({5, 5}, true, {}, false, {}, &ri);
    CHECK(ri.owner[0] == PointerTarget::World && ri.keys_to_world);
    env.frame({5, 5}, false);
    env.frame(env.px_of(*d, "IDC_TEST_OK"), true, {}, false, {}, &ri);
    CHECK(ri.owner[0] == PointerTarget::Dialog);
    env.frame(env.px_of(*d, "IDC_TEST_OK"), false);
    d = env.gui.open("IDD_TEST_CONTROLS");  // modal again

    // Push button: hover SFX, click on release over it, press SFX.
    env.frame({1000, 700}, false);
    env.sfx.clear();
    auto ev = env.click(*d, "IDC_TEST_OK");
    CHECK(has_event(ev, GuiEvent::Kind::Clicked, "IDC_TEST_OK"));
    CHECK(env.sfx.size() >= 2 && env.sfx.front() == "GUI_Common_Dialog_Button_Hilight" && env.sfx.back() == "GUI_Common_Dialog_Button_Press");
    // Press on a button, release elsewhere: no click. Disabled / hidden buttons never click.
    env.frame(env.px_of(*d, "IDC_TEST_OK"), true);
    ev = env.frame({1000, 700}, false);
    CHECK(!has_event(ev, GuiEvent::Kind::Clicked, ""));
    CHECK(!has_event(env.click(*d, "IDC_TEST_OFF"), GuiEvent::Kind::Clicked, ""));
    // Check box toggles; radios are exclusive inside their WS_GROUP group only.
    ev = env.click(*d, "IDC_TEST_CHECK");
    CHECK(has_event(ev, GuiEvent::Kind::Checked, "IDC_TEST_CHECK", 1) && d->get("IDC_TEST_CHECK")->checked);
    env.click(*d, "IDC_TEST_RC");
    env.click(*d, "IDC_TEST_RA");
    CHECK(d->get("IDC_TEST_RA")->checked && d->get("IDC_TEST_RC")->checked);
    ev = env.click(*d, "IDC_TEST_RB");
    CHECK(has_event(ev, GuiEvent::Kind::Checked, "IDC_TEST_RB", 1) && d->get("IDC_TEST_RB")->checked &&
          !d->get("IDC_TEST_RA")->checked && d->get("IDC_TEST_RC")->checked);
    // Slider: press at the right end -> 100; drag to the left edge -> 0; keys.
    {
        const GuiRect r = d->control_rect(static_cast<size_t>(d->find("IDC_TEST_SLIDER")));
        const Vec2f right = env.gui.canvas().canvas_to_pixel(r.max.x - 0.5f, 0.5f * (r.min.y + r.max.y));
        const Vec2f left = env.gui.canvas().canvas_to_pixel(r.min.x + 0.5f, 0.5f * (r.min.y + r.max.y));
        ev = env.frame(right, true);
        CHECK(has_event(ev, GuiEvent::Kind::ValueChanged, "IDC_TEST_SLIDER", 100));
        ev = env.frame(left, true);
        CHECK(has_event(ev, GuiEvent::Kind::ValueChanged, "IDC_TEST_SLIDER", 0));
        env.frame(left, false);
        ev = env.frame(left, false, {vk::kRight});  // focused by the press
        CHECK(d->get("IDC_TEST_SLIDER")->value == 1);
        env.frame(left, false, {vk::kEnd});
        CHECK(d->get("IDC_TEST_SLIDER")->value == 100);
    }
    // List: rows, click selects (SFX), double click, keyboard, scroll.
    {
        std::vector<std::u32string> items;
        for (int i = 0; i < 20; ++i) items.push_back(U"row " + std::u32string(1, static_cast<char32_t>('A' + i)));
        d->set_items("IDC_TEST_LIST", items);
        const GuiRect r = d->control_rect(static_cast<size_t>(d->find("IDC_TEST_LIST")));
        const float rh = static_cast<float>(env.fonts.metrics(env.gui.control_font(*d, static_cast<size_t>(d->find("IDC_TEST_LIST")))).line_height) *
                         env.gui.canvas().units_per_pixel_y;
        const Vec2f row1 = env.gui.canvas().canvas_to_pixel(r.min.x + 20.0f, r.max.y - 2.0f - 1.5f * rh);
        env.sfx.clear();
        env.frame(row1, false);
        ev = env.frame(row1, true);
        CHECK(has_event(ev, GuiEvent::Kind::SelectionChanged, "IDC_TEST_LIST", 1));
        CHECK(!env.sfx.empty() && env.sfx.back() == "GUI_Common_Dialog_Listbox_Entry_Select");
        env.frame(row1, false);
        ev = env.frame(row1, true);
        CHECK(has_event(ev, GuiEvent::Kind::DoubleClicked, "IDC_TEST_LIST", 1));
        env.frame(row1, false);
        ev = env.frame(row1, false, {vk::kDown});
        CHECK(has_event(ev, GuiEvent::Kind::SelectionChanged, "IDC_TEST_LIST", 2));
        for (int i = 0; i < 25; ++i) env.frame(row1, false, {vk::kDown});
        const GuiControl* l = d->get("IDC_TEST_LIST");
        CHECK(l->selected == 19 && l->top > 0 && l->top + 3 >= 19 - 5);  // scrolled to keep the selection visible
    }
    // Combo: open, pick a row, closes.
    {
        d->set_items("IDC_TEST_COMBO", {U"Rebel", U"Empire", U"Underworld"}, 0);
        const GuiRect f = d->control_rect(static_cast<size_t>(d->find("IDC_TEST_COMBO")));
        const Vec2f field = env.gui.canvas().canvas_to_pixel(f.min.x + 20.0f, f.max.y - 7.0f);  // closed field (15 units)
        env.frame(field, false);
        env.frame(field, true);
        env.frame(field, false);
        CHECK(d->get("IDC_TEST_COMBO")->open);
        const float rh = static_cast<float>(env.fonts.metrics(env.gui.control_font(*d, static_cast<size_t>(d->find("IDC_TEST_COMBO")))).line_height) *
                         env.gui.canvas().units_per_pixel_y;
        const Vec2f row2 = env.gui.canvas().canvas_to_pixel(f.min.x + 10.0f, f.max.y - 15.0f - 2.0f - 2.5f * rh);
        env.frame(row2, false);
        ev = env.frame(row2, true);
        env.frame(row2, false);
        CHECK(has_event(ev, GuiEvent::Kind::SelectionChanged, "IDC_TEST_COMBO", 2) && !d->get("IDC_TEST_COMBO")->open);
    }
    // Edit: typed text (WM_CHAR), VK fallback, Backspace, Enter = Submitted.
    {
        env.click(*d, "IDC_TEST_EDIT");
        ev = env.frame({0, 0}, false, {}, false, U"Save 1");
        CHECK(has_event(ev, GuiEvent::Kind::TextChanged, "IDC_TEST_EDIT") && d->get("IDC_TEST_EDIT")->text == U"Save 1");
        env.frame({0, 0}, false, {vk::kBack});
        env.frame({0, 0}, false, {vk::key('x')}, true);
        env.frame({0, 0}, false, {vk::key('y')});
        CHECK(d->get("IDC_TEST_EDIT")->text == U"Save Xy");
        ev = env.frame({0, 0}, false, {vk::kReturn});
        CHECK(has_event(ev, GuiEvent::Kind::Submitted, "IDC_TEST_EDIT") && ev.back().text == U"Save Xy");
    }
    // Focus: Tab order over WS_TABSTOP controls (texts / images / disabled / hidden skipped), Shift+Tab back.
    {
        d->focus("IDC_TEST_OK");
        env.frame({0, 0}, false, {vk::kTab});
        CHECK(d->focus_index() == d->find("IDC_TEST_CHECK"));  // HIDDEN invisible, OFF disabled + NOT WS_TABSTOP
        env.frame({0, 0}, false, {vk::kTab}, true);
        CHECK(d->focus_index() == d->find("IDC_TEST_OK"));
        env.frame({0, 0}, false, {vk::kTab}, true);  // wraps backwards to the last tab stop
        CHECK(d->focus_index() == d->find("IDC_TEST_EDIT"));
        // Space toggles the focused check box; Enter clicks the default button when no button is focused.
        d->focus("IDC_TEST_CHECK");
        const bool before = d->get("IDC_TEST_CHECK")->checked;
        env.frame({0, 0}, false, {vk::kSpace});
        CHECK(d->get("IDC_TEST_CHECK")->checked != before);
        ev = env.frame({0, 0}, false, {vk::kReturn});
        CHECK(has_event(ev, GuiEvent::Kind::Clicked, "IDC_TEST_OK"));
        ev = env.frame({0, 0}, false, {vk::kEscape});
        CHECK(has_event(ev, GuiEvent::Kind::Cancel, ""));
    }
    // Drawing: frame + controls + text land on the dialog's layer; deterministic.
    HudDrawList a, b;
    env.gui.build(a, env.now);
    env.gui.build(b, env.now);
    CHECK(a.total() > 30 && a.digest() == b.digest());
    CHECK(!a.items(0, HudList::Alpha).empty() && !a.items(0, HudList::Text).empty());
    // Gradient text (L_Text Top / Bottom colours).
    bool gradient = false;
    for (const HudQuad& q : a.items(0, HudList::Text)) gradient = gradient || q.gradient;
    CHECK(gradient);
}

void test_menus() {
    Env env;
    CampaignMenu menu;
    {
        CampaignSetInfo story;
        story.set = "Galactic_Civil_War";
        story.text_id = "TEXT_STORY";
        story.factions = {{"Story_Rebel", "Rebel"}, {"Story_Empire", "Empire"}};
        menu.story.push_back(story);
    }
    std::vector<SaveGameInfo> saves = {{"save_a", U"Save A"}, {"save_b", U"Save B"}};
    HotkeyTable keys;
    Bindings binds;
    MenuProviders p;
    p.saves = [&] { return saves; };
    bool exists = false;     // N1 / t09: the save_exists answer
    std::u32string blocker;  // N1 / t09: the save_blocker answer
    p.save_exists = [&](const std::string&) { return exists; };
    p.save_blocker = [&] { return blocker; };
    p.audio = [] { return AudioVolumes{0.5f, 1.0f, 1.0f, 0.25f, true}; };
    p.hotkeys = &keys;
    p.bindings = &binds;
    FrontEnd fe;
    fe.init(env.gui, menu, p);
    fe.show_main_menu();
    std::vector<MenuAction> acts;
    auto step = [&](std::vector<GuiEvent> ev) {
        fe.update(ev, env.last, acts);
    };
    CHECK(fe.top() == "IDD_MAIN_MENU_DIALOG");
    GuiDialog* mm = env.gui.top();
    CHECK(!mm->get("IDC_MAIN_MENU_PLAY_DEMO")->visible && mm->get("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES")->visible);
    step(env.click(*mm, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"));
    CHECK(fe.top() == "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG" && env.gui.open_count() == 1);
    CHECK(!env.gui.top()->get("IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON")->visible);
    step(env.click(*env.gui.top(), "IDC_NEW_CAMPAIGN_BUTTON"));
    CHECK(fe.top() == "IDD_CAMPAIGN_SELECTION_DIALOG");
    GuiDialog* cs = env.gui.top();
    CHECK(cs->get("IDC_CAMPAIGNS_LIST")->items.size() == 1 && cs->get("IDC_CAMPAIGNS_LIST")->selected == 0);
    CHECK(cs->get("IDC_PLAY_FACTION_A_BUTTON")->text == U"Rebel" && cs->get("IDC_STATIC_DIFFICULTY_NAME")->text == U"Normal");
    acts.clear();
    step(env.click(*cs, "IDC_PLAY_FACTION_B_BUTTON"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::StartCampaign && acts[0].campaign == "Story_Empire" &&
          acts[0].faction == "Empire" && acts[0].difficulty == 1);
    // Esc on the selection goes back to the single player menu.
    step(env.frame({0, 0}, false, {vk::kEscape}));
    CHECK(fe.top() == "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG");

    // In game: pause menu -> save (select fills the edit) -> SaveGame; quit -> confirm -> QuitToMainMenu.
    env.gui.close_all();
    fe.show_pause_menu();
    CHECK(fe.pause_menu_open() && env.gui.has_focus());
    {  // QA U11: the shipped layout's empty row is gone: even button spacing, the dialog shorter by that row and still centred.
        const GuiDialog& pm = *env.gui.top();
        const auto top_of = [&](const char* id) { return pm.control_rect(static_cast<size_t>(pm.find(id))).max.y; };
        const float step_y = top_of("IDC_LOAD_GAME") - top_of("IDC_INGAME_OPTIONS");
        CHECK(step_y == 43.0f && top_of("IDC_INGAME_OPTIONS") - top_of("IDC_GAME_OPTIONS_QUIT") == step_y &&
              top_of("IDC_GAME_OPTIONS_QUIT") - top_of("IDC_GAME_OPTIONS_RESUME") == step_y);
        const GuiRect pr = pm.rect();
        CHECK(pr.height() == 285.0f && std::fabs(pr.max.y + pr.min.y) <= 1.0f);
        CHECK(pr.contains(pm.control_rect(static_cast<size_t>(pm.find("IDC_GAME_OPTIONS_RESUME"))).min));
        // The moved button still takes its click.
        const GuiRect quit = pm.control_rect(static_cast<size_t>(pm.find("IDC_GAME_OPTIONS_QUIT")));
        CHECK(pm.control_at({0.5f * (quit.min.x + quit.max.x), 0.5f * (quit.min.y + quit.max.y)}) == pm.find("IDC_GAME_OPTIONS_QUIT"));
    }
    step(env.click(*env.gui.top(), "IDC_SAVE_GAME"));
    GuiDialog* sv = env.gui.top();
    CHECK(fe.top() == "IDD_SAVE_GAME_DIALOG" && sv->get("IDC_SAVE_GAME_LIST")->items.size() == 2);
    {
        std::vector<GuiEvent> e(1);
        e[0].kind = GuiEvent::Kind::SelectionChanged;
        e[0].dialog = "IDD_SAVE_GAME_DIALOG";
        e[0].control = "IDC_SAVE_GAME_LIST";
        e[0].value = 1;
        step(e);
        CHECK(sv->get("IDC_SAVE_GAME_NAME_EDIT")->text == U"save_b");  // N1 / t09: the save's name, not its label
    }
    acts.clear();
    step(env.click(*sv, "IDC_SAVE_GAME_BUTTON"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::SaveGame && acts[0].save == "save_b");
    CHECK(fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    // N1 / t09: Save on an existing name asks first (No keeps the Save dialog, Yes saves and closes it), Delete asks, the
    // host re-lists with refresh_saves(), and a blocked save disables Save and shows the reason in the name field.
    exists = true;
    step(env.click(*env.gui.top(), "IDC_SAVE_GAME"));
    sv = env.gui.top();
    CHECK(fe.top() == "IDD_SAVE_GAME_DIALOG" && sv->get("IDC_SAVE_GAME_BUTTON")->enabled && sv->get("IDC_SAVE_GAME_NAME_EDIT")->text.empty());
    sv->set_text("IDC_SAVE_GAME_NAME_EDIT", U"save_a");
    acts.clear();
    step(env.click(*sv, "IDC_SAVE_GAME_BUTTON"));
    CHECK(acts.empty() && fe.top() == "IDD_DIALOG_BOX_QUIT_CONFIRM");
    CHECK(env.gui.top()->get("IDC_CAPTION")->text == U"Overwrite Save Game?" && env.gui.top()->get("IDC_USER_TEXT_STRING_1")->visible &&
          env.gui.top()->get("IDC_USER_TEXT_STRING_1")->text == U"save_a");
    step(env.click(*env.gui.top(), "IDC_DIALOG_BOX_BUTTON_RIGHT"));
    CHECK(acts.empty() && fe.top() == "IDD_SAVE_GAME_DIALOG");
    step(env.click(*sv, "IDC_SAVE_GAME_BUTTON"));
    step(env.click(*env.gui.top(), "IDC_DIALOG_BOX_BUTTON_LEFT"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::SaveGame && acts[0].save == "save_a" && fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    step(env.click(*env.gui.top(), "IDC_SAVE_GAME"));
    sv = env.gui.top();
    sv->set_selected("IDC_SAVE_GAME_LIST", 0);
    acts.clear();
    step(env.click(*sv, "IDC_DELETE_GAME_BUTTON"));
    CHECK(acts.empty() && fe.top() == "IDD_DIALOG_BOX_QUIT_CONFIRM" && env.gui.top()->get("IDC_CAPTION")->text == U"Delete saved game?");
    step(env.click(*env.gui.top(), "IDC_DIALOG_BOX_BUTTON_LEFT"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::DeleteSave && acts[0].save == "save_a" && fe.top() == "IDD_SAVE_GAME_DIALOG");
    saves.erase(saves.begin());
    fe.refresh_saves();
    CHECK(sv->get("IDC_SAVE_GAME_LIST")->items.size() == 1);
    step(env.click(*sv, "IDC_SAVE_CANCEL_BUTTON"));
    CHECK(fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    blocker = U"a battle is running";
    step(env.click(*env.gui.top(), "IDC_SAVE_GAME"));
    sv = env.gui.top();
    CHECK(!sv->get("IDC_SAVE_GAME_BUTTON")->enabled && !sv->get("IDC_SAVE_GAME_NAME_EDIT")->enabled &&
          sv->get("IDC_SAVE_GAME_NAME_EDIT")->text == U"a battle is running");
    acts.clear();
    {
        std::vector<GuiEvent> e(1);  // Enter in the (disabled) name field
        e[0].kind = GuiEvent::Kind::Submitted;
        e[0].dialog = "IDD_SAVE_GAME_DIALOG";
        e[0].control = "IDC_SAVE_GAME_NAME_EDIT";
        step(e);
    }
    step(env.click(*sv, "IDC_SAVE_GAME_BUTTON"));
    CHECK(acts.empty() && fe.top() == "IDD_SAVE_GAME_DIALOG");
    step(env.click(*sv, "IDC_SAVE_CANCEL_BUTTON"));
    CHECK(fe.top() == "IDD_GAME_OPTIONS_DIALOG");
    blocker.clear();
    exists = false;
    step(env.click(*env.gui.top(), "IDC_GAME_OPTIONS_QUIT"));
    CHECK(fe.top() == "IDD_DIALOG_BOX_QUIT_CONFIRM" && env.gui.top()->get("IDC_DIALOG_BOX_BUTTON_LEFT")->text == U"Yes");
    acts.clear();
    step(env.click(*env.gui.top(), "IDC_DIALOG_BOX_BUTTON_LEFT"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::QuitToMainMenu);
    // Resume with Esc.
    acts.clear();
    step(env.frame({0, 0}, false, {vk::kEscape}));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ResumeGame && !fe.pause_menu_open());

    // Audio options: sliders from the provider, live preview, Cancel reverts.
    GuiDialog* au = env.gui.open("IDD_AUDIO_OPTIONS_DIALOG");
    (void)au;
    env.gui.close("IDD_AUDIO_OPTIONS_DIALOG");
    fe.show_pause_menu();
    {
        // Open audio as the menus do (IDD_IN_GAME_OPTIONS_DIALOG is not in the test script: push through a click event).
        std::vector<GuiEvent> e(1);
        e[0].kind = GuiEvent::Kind::Clicked;
        e[0].dialog = "IDD_IN_GAME_OPTIONS_DIALOG";
        e[0].control = "IDC_IN_GAME_OPTIONS_AUDIO";
        step(e);
    }
    au = env.gui.top();
    CHECK(fe.top() == "IDD_AUDIO_OPTIONS_DIALOG" && au->get("IDC_SLIDER_MASTER_VOLUME")->value == 50 &&
          au->get("IDC_SLIDER_MUSIC_VOLUME")->value == 25);
    acts.clear();
    {
        const GuiRect r = au->control_rect(static_cast<size_t>(au->find("IDC_SLIDER_MASTER_VOLUME")));
        const Vec2f end = env.gui.canvas().canvas_to_pixel(r.max.x - 0.5f, 0.5f * (r.min.y + r.max.y));
        step(env.frame(end, true));
        step(env.frame(end, false));
    }
    CHECK(!acts.empty() && acts.back().kind == MenuAction::Kind::PreviewAudio && acts.back().audio.master == 1.0f);
    acts.clear();
    step(env.click(*au, "IDC_OK"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAudio && acts[0].audio.master == 1.0f &&
          acts[0].audio.music == 0.25f);

    // Keyboard: the focused key-capture edit takes the next key; Accept writes the table and syncs Bindings.
    {
        std::vector<GuiEvent> e(1);
        e[0].kind = GuiEvent::Kind::Clicked;
        e[0].dialog = "IDD_IN_GAME_OPTIONS_DIALOG";
        e[0].control = "IDC_IN_GAME_OPTIONS_KEYBOARD";
        step(e);
    }
    GuiDialog* kb = env.gui.top();
    CHECK(fe.top() == "IDD_KEYBOARD_OPTIONS_DIALOG" && kb->get("IDC_EDIT_STOP")->text == U"H" &&
          kb->get("IDC_EDIT_GROUP_1_SELECT")->text == U"1");
    step(env.click(*kb, "IDC_EDIT_STOP"));
    step(env.frame({0, 0}, false, {vk::key('x')}));
    CHECK(kb->get("IDC_EDIT_STOP")->text == U"X" && keys.binding(hk::kStop).key == 'H');  // not applied yet
    step(env.frame({0, 0}, false, {vk::kControl, vk::key('q')}));  // Ctrl+Q (select like's key) moves here
    CHECK(kb->get("IDC_EDIT_STOP")->text == U"Ctrl+Q");
    acts.clear();
    step(env.click(*kb, "IDC_APPLY"));
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::KeybindingsChanged);
    CHECK(keys.binding(hk::kStop).key == 'Q' && keys.binding(hk::kStop).mod == HotkeyMod::Ctrl &&
          keys.binding(hk::kSelectLike).key == -1);
    CHECK(binds.binding(Action::Stop, 0).key == 'Q' && binds.binding(Action::Stop, 0).mods == kModCtrl);
    CHECK(keyboard_edit_slot("IDC_EDIT_GROUP_0_SELECT") == 9 && keyboard_edit_slot("IDC_EDIT_GROUP_1_SELECT") == 0 &&
          keyboard_edit_slot("IDC_EDIT_SCROLL_UP") == hk::kScrollUp && keyboard_edit_slot("IDC_EDIT_IP") == -1);
    CHECK(hotkey_label({0x7B, HotkeyMod::None}) == U"F12" && hotkey_label({0x6D, HotkeyMod::None}) == U"Numpad -" &&
          hotkey_label({'1', HotkeyMod::Shift}) == U"Shift+1");
}

void test_load_screen() {
    Env env;
    LoadScreen ls;
    LoadScreenInfo info;
    info.planet_name = U"Kuat";
    info.history = {U"line 1", U"line 2"};
    info.planet_icon = "i_planet_kuat.tga";
    float progress = 0.0f;
    ls.set_progress_source([&] { return progress; });
    CHECK(ls.open(env.gui, LoadScreenKind::Skirmish, info));
    GuiDialog* d = env.gui.find("IDD_SKIRMISH_BATTLE_LOAD_DIALOG");
    CHECK(d && !d->get("IDC_BEGIN_BUTTON")->visible && d->get("IDC_STATIC_PLANET_NAME")->text == U"Kuat");
    // QA t16: no live scene around it, the planet's text in the list, the overlay by default, no asset name as the title.
    CHECK(d && d->options().opaque_backdrop && d->get("IDC_LIST_REFERENCE")->items.size() >= 2 &&
          d->get("IDC_LIST_REFERENCE")->items[0] == U"line 1");
    CHECK(gui_no_asset_name(U"_Space_Planet_Felucia_01.ted").empty() && gui_no_asset_name(U"Data\\Art\\x").empty() &&
          gui_no_asset_name(U"Kuat") == U"Kuat" && gui_no_asset_name(U"Mk. II") == U"Mk. II");
    // QA U2: wrap first, then "..."; full-screen art is only the 1024 x 768 kind.
    {
        const int32_t f = env.fonts.font("Arial", 8.0f, 768);
        const std::u32string name = U"Skipray Blastboat Squadron Commander Extra Long x12";
        const auto w = static_cast<float>(env.fonts.measure(f, U"Skipray Blastboat"));
        const std::vector<std::u32string> two = fit_text_lines(env.fonts, f, name, w, 2);
        CHECK(two.size() == 2 && two[0] == U"Skipray Blastboat" && two[1].size() > 3 && two[1].ends_with(U"...") &&
              static_cast<float>(env.fonts.measure(f, two[1])) <= w);
        const std::vector<std::u32string> fits = fit_text_lines(env.fonts, f, U"TIE x7", 1000.0f, 2);
        CHECK(fits.size() == 1 && fits[0] == U"TIE x7");
        // t47: the count survives the cut; the name gives way.
        const std::vector<std::u32string> tail = fit_text_lines(env.fonts, f, name, w, 2, 4);
        CHECK(tail.size() == 2 && tail[0] == U"Skipray Blastboat" && tail[1].ends_with(U"... x12") && tail[1].size() > 7 &&
              static_cast<float>(env.fonts.measure(f, tail[1])) <= w);
        CHECK(fit_text_lines(env.fonts, f, U"TIE x7", 1000.0f, 2, 3)[0] == U"TIE x7");
        GuiImage art;
        art.valid = true;
        art.size = {1024.0f, 768.0f};
        CHECK(gui_full_screen_art(art));
        art.size = {512.0f, 1024.0f};
        CHECK(!gui_full_screen_art(art));
    }
    CHECK(d && !d->get("IDC_PROGRESS0")->visible && d->get("IDC_PROGRESS_SOLO")->visible && d->get("IDC_PLANET_ICON")->image == "i_planet_kuat.tga");
    std::vector<LoadScreenEvent> out;
    progress = 0.4f;
    ls.update({}, out);
    CHECK(out.empty() && ls.loading() && d->get("IDC_PROGRESS_SOLO")->value == 40);
    // While loading, a click where Begin will be does nothing.
    auto ev = env.click(*d, "IDC_BEGIN_BUTTON");
    ls.update(ev, out);
    CHECK(out.empty());
    env.sfx.clear();
    progress = 1.0f;
    ls.update({}, out);
    CHECK(out.size() == 1 && out[0].kind == LoadScreenEvent::Kind::Ready && ls.ready());
    CHECK(d->get("IDC_BEGIN_BUTTON")->visible && d->focus_index() == d->find("IDC_BEGIN_BUTTON"));
    CHECK(env.sfx.size() == 1 && env.sfx[0] == kLoadScreenReadySfx);
    out.clear();
    ev = env.frame({0, 0}, false, {vk::kReturn});  // Enter on the focused Begin button
    ls.update(ev, out);
    CHECK(out.size() == 1 && out[0].kind == LoadScreenEvent::Kind::Begin && !ls.is_open() &&
          env.gui.find("IDD_SKIRMISH_BATTLE_LOAD_DIALOG") == nullptr);
    // Multiplayer: per-player bars; closes with Begin when everybody is done.
    info.multiplayer = true;
    info.players = {U"Alpha", U"Beta"};
    CHECK(ls.open(env.gui, LoadScreenKind::Skirmish, info));
    d = env.gui.find("IDD_SKIRMISH_BATTLE_LOAD_DIALOG");
    CHECK(d && d->get("IDC_PROGRESS0")->visible && d->get("IDC_PROGRESS1")->visible && !d->get("IDC_PROGRESS_SOLO")->visible &&
          d->get("IDC_PLAYER_NAME_1")->text == U"Beta");
    out.clear();
    ls.set_player_progress(0, 1.0f);
    ls.set_player_progress(1, 0.5f);
    ls.update({}, out);
    CHECK(out.empty() && d->get("IDC_PROGRESS1")->value == 50);
    ls.set_player_progress(1, 1.0f);
    ls.update({}, out);
    CHECK(out.size() == 2 && out[1].kind == LoadScreenEvent::Kind::Begin && !ls.is_open());
    CHECK(std::string(load_screen_dialog(LoadScreenKind::SpaceMission)) == "IDD_SPACE_MISSION_LOAD_DIALOG");
    LoadScreenInfo raw;
    raw.planet_name = U"_Space_Planet_Felucia_01.ted";
    CHECK(ls.open(env.gui, LoadScreenKind::Skirmish, raw));
    d = env.gui.find("IDD_SKIRMISH_BATTLE_LOAD_DIALOG");
    CHECK(d && d->get("IDC_STATIC_PLANET_NAME")->text.empty());
    ls.close();
}

const char* kGalacticXml = R"(<?xml version="1.0"?>
<CommandBarComponents>
  <CommandBarComponent Name="i_main_commandbar"><Model_Name>i_galactic_controls.alo</Model_Name><Type>Shell</Type>
    <Model_Offset_X>True</Model_Offset_X><Model_Offset_Y>True</Model_Offset_Y><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="map_shell"><Model_Name>planetary_mode.alo</Model_Name><Type>Shell</Type><Hidden>True</Hidden>
    <Model_Offset_X>False</Model_Offset_X><Model_Offset_Y>False</Model_Offset_Y><Manual_Offset>True</Manual_Offset></CommandBarComponent>
  <CommandBarComponent Name="g_planet_name"><Type>TextButton</Type><Font_Name>EmpireAtWar-Bold</Font_Name>
    <Font_Point_Size>7</Font_Point_Size><Text_Outline>True</Text_Outline><No_Shell>True</No_Shell><Left_Justified>True</Left_Justified>
    <Offset>30 0</Offset></CommandBarComponent>
  <CommandBarComponent Name="b_filters0"><Type>Button</Type><Toggle>True</Toggle></CommandBarComponent>
  <CommandBarComponent Name="g_planet_value"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name>
    <Font_Point_Size>6</Font_Point_Size><Text_Color>255 255 255 180</Text_Color><No_Shell>True</No_Shell>
    <Left_Justified>True</Left_Justified><Offset>30 -11</Offset></CommandBarComponent>
  <CommandBarComponent Name="g_conflict"><Type>Button</Type><Icon_Texture_Name>i_icon_conflict.tga</Icon_Texture_Name>
    <No_Shell>True</No_Shell><Offset>30 -16</Offset></CommandBarComponent>
  <CommandBarComponent Name="g_planet_ring"><Type>Button</Type><Icon_Texture_Name>planet_ring.tga</Icon_Texture_Name>
    <No_Shell>True</No_Shell><Color>255 255 255 255</Color></CommandBarComponent>
  <CommandBarComponent Name="Text_Credits"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name>
    <Font_Point_Size>7</Font_Point_Size><Text_Color>255 212 33 255</Text_Color><Right_Justified>True</Right_Justified></CommandBarComponent>
  <CommandBarComponent Name="b_option_g"><Type>TextButton</Type><Icon_Texture_Name>i_button_options.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="Text_Day_Counter"><Type>TextButton</Type><Font_Name>Arial bold</Font_Name><Font_Point_Size>8</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="balance_pip"><Type>TextButton</Type>
    <Icon_Alternate_Texture_Name>i_bar_fight_red.tga i_bar_fight_blue.tga</Icon_Alternate_Texture_Name>
    <Selected_Texture_Name>i_bar_fight_flash.tga i_bar_fight_flash.tga</Selected_Texture_Name><No_Shell>True</No_Shell>
    <Offset>6 0</Offset><Pixel_Align>True</Pixel_Align><Icon_Offset>3 -1</Icon_Offset></CommandBarComponent>
  <CommandBarComponent Name="Text_Day_Credits"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>7</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="Text_Tech_Level"><Type>TextButton</Type><Font_Name>Arial Bold</Font_Name><Font_Point_Size>8</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="radar_galactic"><Type>Button</Type><Size>175 175</Size></CommandBarComponent>
  <CommandBarComponent Name="Movie_galactic"><Type>Icon</Type><Size>200 200</Size></CommandBarComponent>
  <CommandBarComponent Name="g_radar_blip"><Type>Button</Type><Icon_Texture_Name>i_radar_galactic_planet.tga</Icon_Texture_Name>
    <Selected_Texture_Name>i_radar_galactic_planet_selected.tga</Selected_Texture_Name><No_Shell>True</No_Shell><Base_Layer>3</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="g_radar_view"><Type>Button</Type><Icon_Texture_Name>i_radar_view.tga</Icon_Texture_Name><No_Shell>True</No_Shell>
    <Base_Layer>4</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="planet_info"><Type>TextButton</Type><Icon_Texture_Name>i_reinforcement_button.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="g_planet_fleet"><Type>Button</Type>
    <Blank_Texture_Name> fleet_empty.tga fleet_stacks_01.tga fleet_stacks_02.tga fleet_stacks_03.tga fleet_stacks_04.tga fleet_stacks_05.tga </Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_encyclopedia_rebel_symbol.tga i_encyclopedia_empire_symbol.tga</Icon_Alternate_Texture_Name>
    <No_Shell>True</No_Shell><Size>0.75 5</Size><Offset>3 47</Offset></CommandBarComponent>
  <CommandBarComponent Name="hero_slot_good00"><Type>TextButton</Type><Blank_Texture_Name>rebel_hero_ring.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="build_pad00"><Type>TextButton</Type><Blank_Texture_Name>i_button_units.tga</Blank_Texture_Name>
    <Group>build_pad</Group><Build_Texture_Name>i_button_build.tga</Build_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="build_pad01"><Type>TextButton</Type><Blank_Texture_Name>i_button_units.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_Create_00"><Type>TextButton</Type><Blank_Texture_Name>i_button_units.tga</Blank_Texture_Name>
    <Build_Texture_Name>i_button_build.tga</Build_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_Create_01"><Type>TextButton</Type><Blank_Texture_Name>i_button_units.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_Create_02"><Type>TextButton</Type><Blank_Texture_Name>i_button_units.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue00"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name>
    <Build_Texture_Name>i_button_build.tga</Build_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="q1_0001"><Type>TextButton</Type><Blank_Texture_Name>i_button_unit_frame.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="q1_0002"><Type>TextButton</Type><Blank_Texture_Name>i_button_unit_frame.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="land_fleet_00"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="objective_back"><Type>TextButton</Type><Blank_Texture_Name>i_tooltip_back.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Base_Layer>3</Base_Layer><Color>255 255 255 80</Color><Size>266 14</Size>
    <Default_Offset>383 -180 </Default_Offset><Default_Offset_Widescreen>530 -350 </Default_Offset_Widescreen><Offset>5 2</Offset></CommandBarComponent>
  <CommandBarComponent Name="objective_header_text"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name>
    <Font_Point_Size>8</Font_Point_Size><Text_Color>255 255 255 255</Text_Color><Text_Color2>255 0 0 255</Text_Color2>
    <No_Shell>True</No_Shell><Base_Layer>4</Base_Layer><Left_Justified>True</Left_Justified><Size>5 18</Size></CommandBarComponent>
  <CommandBarComponent Name="g_hero"><Type>Button</Type><No_Shell>True</No_Shell><Scale>1.0</Scale>
    <Icon_Alternate_Texture_Name>i_icon_hero_you.tga i_icon_hero_enemy.tga i_icon_rebel_hero.tga i_icon_empire_hero.tga i_icon_underworld_hero.tga</Icon_Alternate_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="objective_text"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <Text_Color>255 255 255 255</Text_Color><Text_Color2>128 128 128 255</Text_Color2><No_Shell>True</No_Shell><Base_Layer>4</Base_Layer>
    <Left_Justified>True</Left_Justified><Size>40 16</Size></CommandBarComponent>
  <CommandBarComponent Name="objective_icon"><Type>TextButton</Type><Icon_Texture_Name>checked_box.tga</Icon_Texture_Name>
    <Icon_Alternate_Texture_Name>failed_box.tga</Icon_Alternate_Texture_Name><Size>16 16</Size><Base_Layer>4</Base_Layer></CommandBarComponent>
</CommandBarComponents>
)";

// GALACTIC_DIALOGS batch: battle prompt (§1), holocron flash (§2), auto-resolve text (§3), organiser gating (§4), command
// bar details (§5), shell anchors.
const char* kGalacticDialogsXml = R"(<?xml version="1.0"?>
<CommandBarComponents>
  <CommandBarComponent Name="i_main_organize"><Model_Name>i_galactic_organize.alo</Model_Name><Type>Shell</Type>
    <Model_Offset_X>False</Model_Offset_X><Model_Offset_Y>True</Model_Offset_Y><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="pending_battle_shell"><Model_Name>i_attack_mode_choice.alo</Model_Name><Type>Shell</Type>
    <Model_Offset_X>False</Model_Offset_X><Model_Offset_Y>True</Model_Offset_Y><Manual_Offset>True</Manual_Offset>
    <Tutorial_Scene>True</Tutorial_Scene><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="story_pending_battle_shell"><Model_Name>i_attack_mode_single.alo</Model_Name><Type>Shell</Type>
    <Model_Offset_X>False</Model_Offset_X><Model_Offset_Y>True</Model_Offset_Y><Manual_Offset>True</Manual_Offset>
    <Tutorial_Scene>True</Tutorial_Scene><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="text_attack_choice"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name>
    <Font_Point_Size>7</Font_Point_Size><Text_Color>150 187 227 255</Text_Color><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="choice_button_left"><Type>TextButton</Type><Mouse_Over_Texture_Name>i_attack_mode_rollover.tga</Mouse_Over_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>7</Font_Point_Size><Text_Color>255 64 64 255</Text_Color><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="choice_button_right"><Type>TextButton</Type><Mouse_Over_Texture_Name>i_attack_mode_rollover.tga</Mouse_Over_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>7</Font_Point_Size><Text_Color>255 64 64 255</Text_Color><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="graph_left"><Type>Button</Type>
    <Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga fleet_stacks_02.tga fleet_stacks_03.tga fleet_stacks_04.tga fleet_stacks_05.tga</Blank_Texture_Name>
    <Offset>3 0</Offset><Size>0.75 0</Size><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="graph_right"><Type>Button</Type>
    <Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga fleet_stacks_02.tga fleet_stacks_03.tga fleet_stacks_04.tga fleet_stacks_05.tga</Blank_Texture_Name>
    <Offset>3 0</Offset><Size>0.75 0</Size><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="text_attack_single"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="attack_button_single"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name><Base_Layer>9</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="Text_Planet"><Type>TextButton</Type><Font_Name>EmpireAtWar-Bold</Font_Name><Font_Point_Size>9</Font_Point_Size><Max_Text_Width>110</Max_Text_Width></CommandBarComponent>
  <CommandBarComponent Name="Text_Planetary_Income"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name></CommandBarComponent>
  <CommandBarComponent Name="Text_Allegiance_A"><Type>Icon</Type>
    <Icon_Alternate_Texture_Name>i_rebel_symbol.tga i_empire_symbol.tga i_underworld_symbol.tga</Icon_Alternate_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_story_arc_g"><Type>Button</Type><Blank_Texture_Name>i_holocron.tga</Blank_Texture_Name>
    <Flash_Texture_Name>i_holocron_flash.tga</Flash_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_Create_00"><Type>TextButton</Type><Blank_Texture_Name>i_button_pad_rebel.tga i_button_pad_empire.tga</Blank_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>6</Font_Point_Size><Text_Outline>True</Text_Outline></CommandBarComponent>
  <CommandBarComponent Name="b_Create_01"><Type>TextButton</Type><Blank_Texture_Name>i_button_pad_rebel.tga i_button_pad_empire.tga</Blank_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>6</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="queue00"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue01"><Type>Button</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue02"><Type>Button</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue03"><Type>Button</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue04"><Type>Button</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue05"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram_land.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue06"><Type>Button</Type><Blank_Texture_Name>i_button_hologram_land.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue07"><Type>Button</Type><Blank_Texture_Name>i_button_hologram_land.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue08"><Type>Button</Type><Blank_Texture_Name>i_button_hologram_land.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="queue09"><Type>Button</Type><Blank_Texture_Name>i_button_hologram_land.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="g_planet_name"><Type>TextButton</Type><No_Shell>True</No_Shell><Offset>30 0</Offset></CommandBarComponent>
  <CommandBarComponent Name="g_planet_fleet"><Type>Button</Type>
    <Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga fleet_stacks_02.tga fleet_stacks_03.tga fleet_stacks_04.tga fleet_stacks_05.tga</Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_encyclopedia_rebel_symbol.tga i_encyclopedia_empire_symbol.tga</Icon_Alternate_Texture_Name>
    <No_Shell>True</No_Shell><Size>0.75 0</Size><Offset>3 47</Offset><Default_Offset>36 42</Default_Offset></CommandBarComponent>
  <CommandBarComponent Name="g_planet_land_forces"><Type>Button</Type>
    <Blank_Texture_Name>fleet_empty.tga fleet_stacks_01.tga fleet_stacks_02.tga fleet_stacks_03.tga fleet_stacks_04.tga fleet_stacks_05.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Size>0.75 0</Size><Offset>3 5</Offset></CommandBarComponent>
</CommandBarComponents>
)";

void test_galactic_dialogs() {
    ComponentTable table;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kGalacticDialogsXml), "gd.xml");
    table.parse(doc.root());
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 256;
    for (const char* n : {"fleet_empty.tga", "fleet_stacks_01.tga", "fleet_stacks_02.tga", "fleet_stacks_03.tga", "fleet_stacks_04.tga",
                          "fleet_stacks_05.tga"})
        atlas.add({n, 0, 0, 36, 45, true});
    for (const char* n : {"i_button_pad_rebel.tga", "i_button_pad_empire.tga", "i_button_units.tga"}) atlas.add({n, 40, 0, 56, 56, true});
    for (const char* n : {"i_button_hologram.tga", "i_button_hologram_land.tga"}) atlas.add({n, 100, 0, 79, 79, true});
    atlas.add({"i_empire_symbol.tga", 180, 0, 21, 21, true});
    atlas.add({"i_holocron.tga", 200, 0, 24, 25, true});
    atlas.add({"i_holocron_flash.tga", 224, 0, 24, 25, true});
    auto raster = make_box_rasterizer();
    FontCache fonts(*raster);
    TextDatabase td;
    td.add("TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING", u": SPACE BATTLE IMMINENT!");
    td.add("TEXT_BUTTON_BEGIN", u"Begin");
    td.add("TEXT_BUTTON_RESOLVE_BATTLE", u"Auto-Resolve");
    td.add("TEXT_AR_SPACE_WIN_MINOR", u"##0 minor space victory at ##1. Ships lost: ##2");
    td.add("TEXT_AR_HERO_LOSS", u"##0 defeated at ##1");
    GalacticHud hud;
    hud.init(table, atlas, fonts, &td);
    hud.resize(1024, 768);

    // Shell anchors: organiser origin = canvas centre (top edge - 384), attack banners: model y 92 on the top edge.
    {
        const ShellBone b[1] = {{"x", 10.0f, 50.0f, 0.0f, {}, {}}};
        const Vec2f org = shell_anchor_offset(table[static_cast<size_t>(table.find("i_main_organize"))], b, hud.canvas());
        const Vec2f pend = shell_anchor_offset(table[static_cast<size_t>(table.find("pending_battle_shell"))], b, hud.canvas());
        CHECK(org.x == 0.0f && org.y == 0.0f && pend.x == 0.0f && pend.y == 384.0f - 92.0f);
    }

    std::vector<GalacticPlanetInfo> planets(1);
    planets[0].planet = 55;
    planets[0].name = U"Kuat";
    planets[0].credits = 120;
    planets[0].income = -1250;
    planets[0].selected = true;
    planets[0].owner_color = Rgba8{60, 120, 255, 255};
    planets[0].allegiance = 1;
    GalacticHudFrame f;
    GalacticCameraController cam;
    f.view = make_view(cam.params(4.0f / 3.0f), 1024, 768);
    f.planets = planets;
    f.holocron_flash = true;
    f.providers.build_options = [](uint32_t, std::vector<GalacticBuildOption>& out) {
        GalacticBuildOption a;
        a.type_index = 3;
        a.icon = "i_button_units.tga";
        a.structure = true;
        a.cost = 1500;
        a.space = true;
        out.push_back(a);
        a.space = false;
        a.enabled = false;
        out.push_back(a);
    };
    f.providers.build_queue = [](std::vector<HudQueueEntry>& out) {
        HudQueueEntry e;
        e.type_index = 1;
        out.push_back(e);  // space -> queue00
        e.land = true;
        out.push_back(e);  // ground -> queue05
        e.land = false;
        out.push_back(e);  // space -> queue01
    };
    hud.build(f);
    auto state = [&](const char* n) { return hud.buttons().find_state(static_cast<uint32_t>(table.find(n))); };
    auto has_hit = [&](const char* n) {
        const int32_t ci = table.find(n);
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == ci) return true;
        return false;
    };
    // §5.3 resource panel: planet name in capitals and owner colour, owner emblem, holocron flash.
    {
        const ButtonRuntime* tp = state("Text_Planet");
        const ButtonRuntime* al = state("Text_Allegiance_A");
        const ButtonRuntime* ho = state("b_story_arc_g");
        CHECK(tp && tp->text == U"KUAT" && tp->text_color_set && tp->text_color.b == 255 && tp->text_color.r == 60);
        // t45: a negative amount still shows; only the sentinel blanks it.
        const ButtonRuntime* pi = state("Text_Planetary_Income");
        CHECK(pi && pi->text == U"$ -1,250" && pi->text_color_set);
        CHECK(al && !al->hidden && al->alt == 1 && ho && ho->flash);
        // D12: the name's line is centred on the bone (25.jpg), not hung half a line below it.
        const Vec2f bone = hud.layout().placement(static_cast<uint32_t>(table.find("Text_Planet"))).center;
        float name_top = -1e9f;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Text))
            if (std::fabs(0.5f * (q.min.x + q.max.x) - bone.x) < 20.0f && q.min.y < bone.y + 20.0f) name_top = std::max(name_top, q.max.y);
        CHECK(name_top > bone.y + 1.0f);
        // t47: a name wider than Max_Text_Width stays on one line, inside it.
        auto below_name = [&] {  // the owner's colour on the row under the name's line
            int32_t n = 0;
            for (const HudQuad& q : hud.draw_list().items(0, HudList::Text))
                if (q.color.r == 60 && q.color.b == 255 && std::fabs(0.5f * (q.min.x + q.max.x) - bone.x) < 60.0f &&
                    0.5f * (q.min.y + q.max.y) < bone.y - 8.0f && 0.5f * (q.min.y + q.max.y) > bone.y - 30.0f)
                    ++n;
            return n;
        };
        const int32_t below_short = below_name();
        const float max_w = table[static_cast<size_t>(table.find("Text_Planet"))].max_text_width;
        const int32_t name_font = fonts.font("EmpireAtWar-Bold", 9.0f, hud.canvas().screen_height);
        planets[0].name = U"KUAT";
        while (static_cast<float>(fonts.measure(name_font, planets[0].name)) * hud.canvas().units_per_pixel_x < 1.5f * max_w) planets[0].name += U" KUAT";
        hud.build(f);
        const float limit = 0.5f * max_w + 2.0f;
        float left = 1e9f, right = -1e9f;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Text))
            if (q.color.r == 60 && q.color.b == 255 && std::fabs(0.5f * (q.min.x + q.max.x) - bone.x) < 150.0f &&
                std::fabs(0.5f * (q.min.y + q.max.y) - bone.y) < 8.0f) {
                left = std::min(left, q.min.x);
                right = std::max(right, q.max.x);
            }
        CHECK(below_name() == below_short && left >= bone.x - limit && right <= bone.x + limit);
        planets[0].name = U"Kuat";
        hud.build(f);
    }
    // §5.1 pads: blue (index 1) for the space option, amber (0) for the ground one; cost not in text2 (drawn at the bottom).
    {
        const ButtonRuntime* c0 = state("b_Create_00");
        const ButtonRuntime* c1 = state("b_Create_01");
        CHECK(c0 && c1 && c0->alt == 1 && c1->alt == 0 && c0->text2.empty() && hud.stats().build_buttons == 2);
    }
    // §5.2 queue: space entries on queue00.., ground on queue05...
    CHECK(has_hit("queue00") && has_hit("queue01") && has_hit("queue05") && !has_hit("queue02") && !has_hit("queue06") &&
          hud.stats().queue_entries == 3);
    {
        const ComponentPlacement& q0 = hud.layout().placement(static_cast<uint32_t>(table.find("queue00")));
        CHECK(q0.placed && std::fabs(q0.center.y - (-384.0f + 174.0f)) < 1e-3f);  // dome row (fallback = the bone)
    }
    // §4 organiser: shown with organize_open and the advanced map closed.
    CHECK(hud.stats().organize_shown);
    {
        GalacticHudFrame g = f;
        g.advanced_map_open = true;
        hud.build(g);
        CHECK(!hud.stats().organize_shown);
        bool org_off = false;
        for (const GalacticShellVisibility& v : hud.shell_visibility())
            org_off = org_off || (v.component == static_cast<uint32_t>(table.find("i_main_organize")) && !v.visible);
        CHECK(org_off);
    }
    // §1 battle prompt: none by default.
    CHECK(!hud.stats().battle_prompt && !has_hit("choice_button_left"));
    auto click = [&](const char* n, std::vector<HudAction>& acts) {
        const int32_t ci = table.find(n);
        RoutedInput ri;
        ri.hover_component = ci;
        ri.hud = {{HudPointerEvent::Kind::Press, Button::Left, ci, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, ci, 0, 0},
                  {HudPointerEvent::Kind::Click, Button::Left, ci, 0, 0}};
        std::vector<commands::EventData> cmds;
        hud.update_input(ri, false, false, 1.0, cmds, &acts);
    };
    {
        GalacticHudFrame g = f;
        g.battle.active = true;
        g.battle.planet = U"Kuat";
        g.battle.left = {"i_button_units.tga", 12, -1};
        g.battle.right = {"", 3, 2};
        hud.build(g);
        CHECK(hud.stats().battle_prompt && !hud.stats().battle_prompt_shell && hud.stats().battle_graphs == 2 &&
              has_hit("choice_button_left") && has_hit("choice_button_right") && !has_hit("attack_button_single"));
        bool pend_on = false, story_off = false;
        for (const GalacticShellVisibility& v : hud.shell_visibility()) {
            pend_on = pend_on || (v.component == static_cast<uint32_t>(table.find("pending_battle_shell")) && v.visible);
            story_off = story_off || (v.component == static_cast<uint32_t>(table.find("story_pending_battle_shell")) && !v.visible);
        }
        CHECK(pend_on && story_off);
        // Title at the top-centre banner spot (8 units below the top edge), buttons 45 below.
        const ComponentPlacement& t = hud.layout().placement(static_cast<uint32_t>(table.find("text_attack_choice")));
        const ComponentPlacement& l = hud.layout().placement(static_cast<uint32_t>(table.find("choice_button_left")));
        CHECK(t.placed && std::fabs(t.center.y - 376.0f) < 1e-3f && std::fabs(l.center.y - 339.0f) < 1e-3f && l.center.x < 0.0f);
        std::vector<HudAction> acts;
        click("choice_button_left", acts);
        click("choice_button_right", acts);
        CHECK(acts.size() == 2 && acts[0].kind == HudAction::Kind::BattleBegin && acts[1].kind == HudAction::Kind::BattleAutoResolve);
        g.battle.single = true;
        hud.build(g);
        CHECK(hud.stats().battle_prompt && has_hit("attack_button_single") && !has_hit("choice_button_right"));
        acts.clear();
        click("attack_button_single", acts);
        CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::BattleBegin);
    }
    // §2.1 / §3.1 screen text and the auto-resolve result line.
    {
        const std::u32string ar = auto_resolve_text(&td, true, true, 2, U"Empire", U"Kuat", 4);
        CHECK(ar == U"Empire minor space victory at Kuat. Ships lost: 4");
        CHECK(auto_resolve_hero_loss_text(&td, U"Boba Fett", U"Kuat") == U"Boba Fett defeated at Kuat");
        CHECK(auto_resolve_text(nullptr, false, false, 0, U"Rebels", U"Kuat", 9) == U"Rebels major land defeat at Kuat. Units lost: 9");
        const GalacticScreenText lines[2] = {{U"The mission holocron has been updated.", Rgba8{255, 255, 255, 255}, 255},
                                             {ar, kAutoResolveTextColor, 255}};
        GalacticHudFrame g = f;
        // Text quads per layer without the lines, then with them: the lines land on kScreenTextLayer only, a layer the
        // frame order draws before the GUI dialogs (an open Save dialog covers them).
        hud.build(f);
        std::vector<size_t> before(kHudLayers);
        for (uint32_t l = 0; l < kHudLayers; ++l) before[l] = hud.draw_list().items(l, HudList::Text).size();
        g.screen_text = lines;
        hud.build(g);
        CHECK(hud.stats().screen_text_lines == 2);
        for (uint32_t l = 0; l < kHudLayers; ++l)
            CHECK((hud.draw_list().items(l, HudList::Text).size() > before[l]) == (l == kScreenTextLayer));
        bool text_drawn = false, dialogs_after = false;
        for (const HudFrameStep& step : hud_frame_steps()) {
            if (step.kind == HudStepKind::Layers && step.first <= kScreenTextLayer && kScreenTextLayer <= step.last) text_drawn = true;
            if (step.kind == HudStepKind::Dialogs) dialogs_after = text_drawn;
        }
        CHECK(dialogs_after);
    }
    // GALAXY_MAP_PRESENTATION §1: fleet buttons around the planet (slot 0 / 2 at -/+36, 42; slot 1 at 0, 47), land forces
    // at (0, 5); enemy fleet to slot 0 and the own slot-0 fleet to slot 2 when the orbit is not the local player's; the
    // stack depth from the unit count; no faction emblem.
    {
        std::vector<GalacticPlanetInfo> ps(1);
        ps[0].planet = 77;
        ps[0].name = U"Nal Hutta";
        ps[0].orbit_local = false;
        GalacticPlanetStack own;
        own.slot = 0;
        own.units = 12;  // 3 layers
        own.icon = "i_button_units.tga";
        own.owner_color = {40, 90, 220, 255};  // R2 t01: §1.4 secondary colour = the owner faction's [M: tints the stack texture]
        GalacticPlanetStack foe;
        foe.slot = 1;
        foe.enemy = true;
        foe.owner_color = {200, 40, 40, 255};
        foe.units = 4;  // 1 layer
        GalacticPlanetStack land;
        land.land = true;
        land.units = 30;  // capped at 5
        ps[0].stacks = {own, foe, land};
        GalacticHudFrame g = f;
        g.planets = ps;
        g.providers = {};
        hud.build(g);
        CHECK(hud.stats().stack_icons == 3);
        std::vector<Vec2f> stacks;
        std::vector<Rgba8> tints;
        bool emblem = false;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Alpha)) {
            const float w = q.max.x - q.min.x, h = q.max.y - q.min.y;
            if (std::fabs(w - 36.0f) < 0.01f && std::fabs(h - 45.0f) < 0.01f) {  // the art at Scale 1 (base 40.jpg)
                stacks.push_back({0.5f * (q.min.x + q.max.x), 0.5f * (q.min.y + q.max.y)});
                tints.push_back(q.color);
            }
        }
        {  // the stack textures carry the owners' colours (own blue, enemy red, land white); the unit icon stays white
            int blue = 0, red = 0, white = 0;
            for (const Rgba8& c : tints) {
                blue += c.r == 40 && c.g == 90 && c.b == 220 ? 1 : 0;
                red += c.r == 200 && c.g == 40 && c.b == 40 ? 1 : 0;
                white += c.r == 255 && c.g == 255 && c.b == 255 ? 1 : 0;
            }
            CHECK(blue == 1 && red == 1 && white == 1);
        }
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Alpha)) emblem = emblem || (q.max.x - q.min.x) == 15.75f;  // 21 x 0.75
        std::sort(stacks.begin(), stacks.end(), [](Vec2f a, Vec2f b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        CHECK(stacks.size() == 3 && !emblem);
        if (stacks.size() == 3) {
            // x order: slot 0 (enemy, -36), land (0, +5), slot 2 (own, +36).
            CHECK(std::fabs(stacks[2].x - stacks[0].x - 72.0f) < 0.01f && std::fabs(stacks[0].y - stacks[2].y) < 0.01f &&
                  std::fabs(stacks[0].y - stacks[1].y - 37.0f) < 0.01f);
        }
    }
    std::printf("galactic dialogs: prompt / queue / pads / organiser / resource panel / planet stacks ok\n");
}

void test_galactic_hud() {
    CHECK(planet_label_alpha(0, 0.5f) == 255.0f && planet_label_alpha(1, 0.0f) == 255.0f &&
          std::fabs(planet_label_alpha(1, 0.05f) - 127.5f) < 1e-3f && planet_label_alpha(1, 0.2f) == 0.0f &&
          planet_label_alpha(2, 0.9f) == 0.0f && std::fabs(planet_label_alpha(2, 0.95f) - 127.5f) < 1e-2f &&
          planet_label_alpha(2, 1.0f) == 255.0f && planet_label_alpha(3, 0.5f) == 0.0f);
    ComponentTable table;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kGalacticXml), "g.xml");
    table.parse(doc.root());
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 256;
    for (const char* n : {"i_button_units.tga", "i_button_hologram.tga", "i_button_unit_frame.tga", "i_button_options.tga",
                          "i_reinforcement_button.tga"})
        atlas.add({n, 0, 0, 40, 40, true});
    atlas.add({"i_icon_conflict.tga", 0, 0, 12, 12, true});
    atlas.add({"planet_ring.tga", 0, 0, 64, 64, true});
    atlas.add({"i_icon_empire_hero.tga", 140, 40, 14, 14, true});
    atlas.add({"i_icon_rebel_hero.tga", 160, 40, 14, 14, true});
    atlas.add({"rebel_hero_ring.tga", 64, 0, 32, 32, true});
    atlas.add({"fleet_stacks_03.tga", 96, 0, 24, 12, true});
    atlas.add({"i_bar_fight_red.tga", 120, 0, 6, 10, true});
    atlas.add({"i_bar_fight_blue.tga", 126, 0, 6, 10, true});
    atlas.add({"i_bar_fight_flash.tga", 132, 0, 6, 10, true});
    auto raster = make_box_rasterizer();
    FontCache fonts(*raster);
    GalacticHud hud;
    hud.init(table, atlas, fonts);
    hud.resize(1024, 768);
    GalacticCameraController cam;
    std::vector<GalacticPlanetInfo> planets(3);
    for (size_t i = 0; i < planets.size(); ++i) {
        planets[i].planet = 100 + static_cast<uint32_t>(i);
        planets[i].position = {-200.0f + 200.0f * static_cast<float>(i), 0.0f, 0.0f};
        planets[i].name = U"Planet";
        planets[i].credits = 50;
    }
    planets[1].selected = true;
    planets[1].conflict = true;
    planets[2].visible = false;
    GalacticHudFrame f;
    CameraParams cp = cam.params(4.0f / 3.0f);
    f.view = make_view(cp, 1024, 768);
    f.planets = planets;
    f.credits = 12345;
    f.providers.build_options = [](uint32_t planet, std::vector<GalacticBuildOption>& out) {
        GalacticBuildOption o;
        o.type_index = static_cast<uint16_t>(planet);
        o.icon = "i_button_units.tga";
        out.push_back(o);
    };
    f.providers.build_queue = [](std::vector<HudQueueEntry>& out) {
        HudQueueEntry e;
        e.type_index = 7;
        e.location = 101;
        e.progress = 0.5f;
        out.push_back(e);
    };
    f.providers.fleets = [](uint32_t, std::vector<GalacticFleet>& out) {
        GalacticFleet fl;
        fl.fleet = 900;
        fl.units = {{901, "i_button_units.tga", 2, false}, {902, "i_button_units.tga", 1, false}};
        out.push_back(fl);
    };
    f.providers.objectives = [](std::vector<HudObjective>& out) {
        out.push_back({"TEXT_OBJ_1", U"Conquer Kuat and hold it for three days", HudObjective::State::Active});
        out.push_back({"TEXT_OBJ_2", U"Done", HudObjective::State::Complete});
    };
    hud.build(f);
    // FIXPASS_GALACTIC §4.2, capture C2: the advisor loop in Movie_galactic, bone at its bottom-left corner; nothing without a loop.
    {
        CHECK(!hud.stats().advisor_movie);
        const size_t before = hud.draw_list().total();
        GalacticHudFrame m = f;
        m.advisor_page = 30;
        hud.build(m);
        CHECK(hud.stats().advisor_movie && hud.draw_list().total() == before + 1);
        bool found = false;
        for (uint32_t l = 0; l < kHudLayers; ++l)
            for (const HudQuad& q : hud.draw_list().items(l, HudList::Alpha))
                if (q.page == 30)
                    found = std::fabs(q.max.x - q.min.x - 200.0f) < 1e-3f && std::fabs(q.max.y - q.min.y - 200.0f) < 1e-3f &&
                            std::fabs(q.min.x - (-0.5f * hud.canvas().width + 51.0f)) < 1e-3f &&
                            std::fabs(q.min.y - (-0.5f * hud.canvas().height + 288.0f)) < 1e-3f;
        CHECK(found);
        hud.build(f);
        CHECK(!hud.stats().advisor_movie);
    }
    const GalacticHudStats& s = hud.stats();
    std::printf("galactic: %u labels, %u icons, %u buttons, %u build, %u queue, %u fleet, %u objectives, %u hits\n", s.labels,
                s.label_icons, s.buttons, s.build_buttons, s.queue_entries, s.fleet_slots, s.objectives, s.hit_boxes);
    CHECK(s.labels == 2 && s.label_icons == 1 && s.build_buttons == 1 && s.queue_entries == 1 && s.fleet_slots == 2 &&
          s.objectives == 2 && hud.selected_planet() == 1);
    // t49: g_planet_ring round every visible planet, at its atlas size, selected or not, fogged too; g_planet_value only with
    // the b_filters0 toggle on.
    {
        const uint32_t rings = hud.stats().planet_rings;
        CHECK(rings == 2 && hud.stats().planet_values == 0);
        bool sized = false;
        for (const HudQuad& q : hud.draw_list().items(0, HudList::Alpha))
            sized = sized || (std::fabs(q.max.x - q.min.x - 64.0f) < 1e-3f && std::fabs(q.max.y - q.min.y - 64.0f) < 1e-3f && q.color.a == 173);
        CHECK(sized);
        const int32_t filter = table.find("b_filters0");
        CHECK(filter >= 0);
        if (filter >= 0) {
            hud.buttons().state(static_cast<uint32_t>(filter)).selected = true;
            hud.build(f);
            CHECK(hud.stats().planet_values == 2 && hud.stats().planet_rings == 2);
            hud.buttons().state(static_cast<uint32_t>(filter)).selected = false;
            hud.build(f);
        }
    }
    // [port: Gabriel, original screenshot] resource texts: "Day: 1" (TEXT_CYCLES), "Tech: 3" (TEXT_TECH), credits and income
    // with thousands separators; minimap blips for the visible planets and the camera footprint.
    {
        TextDatabase td;
        td.add("TEXT_CYCLES", u"Day");
        td.add("TEXT_TECH", u"Tech:");
        GalacticHud h2;
        h2.init(table, atlas, fonts, &td);
        h2.resize(1024, 768);
        GalacticHudFrame g = f;
        g.credits = 13000;
        g.income = 1668;
        g.day = 1;
        g.tech = 3;
        h2.build(g);
        auto text_of = [&](const char* n) {
            const ButtonRuntime* r = h2.buttons().find_state(static_cast<uint32_t>(table.find(n)));
            return r ? r->text : std::u32string();
        };
        // Credit amounts carry the credits symbol ('$' in the EmpireAtWar fonts).
        CHECK(text_of("Text_Credits") == U"$ 13,000" && text_of("Text_Day_Credits") == U"$ 1,668");
        CHECK(text_of("Text_Day_Counter") == U"Day: 1" && text_of("Text_Tech_Level") == U"Tech: 3");
        CHECK(h2.stats().radar_blips == 2 && h2.stats().radar_view);
        // Playtest: an opaque radar rectangle on layer 0 (under the shells) so the world (trade routes) never shows
        // through the radar box; drawn even with no planets.
        {
            const auto op = h2.draw_list().items(0, HudList::Opaque);
            bool box = false;
            for (const HudQuad& q : op)
                box = box || (q.color.a == 255 && std::fabs((q.max.x - q.min.x) - 175.0f) < 0.01f &&
                              std::fabs((q.max.y - q.min.y) - 175.0f) < 0.01f && q.page == kHudPageWhite);
            CHECK(h2.stats().radar_backdrop && box);
            GalacticHudFrame empty = g;
            empty.planets = {};
            h2.build(empty);
            CHECK(h2.stats().radar_backdrop && h2.stats().radar_blips == 0 && h2.draw_list().items(0, HudList::Opaque).size() == 1);
            h2.build(g);
        }
        // Day pips (HUD_GAPS §4): hidden without a cycle fraction; 10 pips, 0..k blue, pip k flashing.
        {
            CHECK(h2.stats().day_pips == 0);
            GalacticHudFrame d = g;
            d.cycle_fraction = 0.35f;
            h2.build(d);
            std::printf("day pips: %u (%u lit)\n", h2.stats().day_pips, h2.stats().day_pips_lit);
            CHECK(h2.stats().day_pips == 10 && h2.stats().day_pips_lit == 4);
            auto count_u = [&](float u0) {
                uint32_t n = 0;
                for (uint32_t l = 0; l < 13; ++l)
                    for (const HudQuad& q : h2.draw_list().items(l, HudList::Alpha))
                        if (std::fabs(q.uv0.x - u0 / 256.0f) < 1e-4f && std::fabs(q.max.x - q.min.x - 6.0f) < 0.01f) ++n;
                return n;
            };
            CHECK(count_u(126) == 3 && count_u(132) == 1 && count_u(120) == 6);
            d.day_pip_flash = false;
            d.cycle_fraction = 1.5f;  // clamps to the last pip
            h2.build(d);
            CHECK(h2.stats().day_pips_lit == 10 && count_u(132) == 0 && count_u(126) == 10);
            CHECK(h2.stats().day_pip_anchor == GalacticHudStats::DayPipAnchor::Fallback);
            // HUD_GAPS §4.2: first pip on fight_bar_g of i_main_commandbar; with the advanced planet map open on day_clock
            // of map_shell (bones from the real i_galactic_controls.alo / planetary_mode.alo).
            GalacticHud h3;
            h3.init(table, atlas, fonts, &td);
            const auto bar = static_cast<uint32_t>(table.find("i_main_commandbar"));
            const auto map = static_cast<uint32_t>(table.find("map_shell"));
            CHECK(table.find("map_shell") >= 0);
            ShellBone fb;
            fb.name = "fight_bar_g";
            fb.x = 143.0f;
            fb.y = 236.0f;
            ShellBone dc;
            dc.name = "day_clock";
            dc.x = -395.0108f;
            dc.y = -163.0f;
            h3.layout().set_shell_bones(bar, {fb});
            h3.resize(1024, 768);
            d.cycle_fraction = 0.35f;
            auto origin_of = [&](uint32_t shell, const ShellBone& b) {
                const Vec2f o = h3.layout().shell_offset(shell);
                const Vec2f px = h3.canvas().canvas_to_pixel(b.x + o.x, b.y + o.y);
                return h3.canvas().pixel_to_canvas(std::floor(px.x) + 0.5f, std::floor(px.y) + 0.5f);
            };
            auto near2 = [](Vec2f a, Vec2f b) { return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f; };
            h3.build(d);
            CHECK(h3.stats().day_pip_anchor == GalacticHudStats::DayPipAnchor::FightBar &&
                  near2(h3.stats().day_pip_origin, origin_of(bar, fb)) && h3.stats().day_pips == 10);
            d.advanced_map_open = true;  // map_shell bones not loaded: [port] stays on fight_bar_g
            h3.build(d);
            CHECK(h3.stats().day_pip_anchor == GalacticHudStats::DayPipAnchor::FightBar);
            h3.layout().set_shell_bones(map, {dc});
            h3.resize(1024, 768);
            h3.build(d);
            const Vec2f mo = origin_of(map, dc);
            std::printf("day pips on day_clock at (%.1f, %.1f)\n", static_cast<double>(mo.x), static_cast<double>(mo.y));
            CHECK(h3.stats().day_pip_anchor == GalacticHudStats::DayPipAnchor::DayClock &&
                  near2(h3.stats().day_pip_origin, mo) && h3.stats().day_pips == 10 && h3.stats().day_pips_lit == 4);
            // The pips run left to right from day_clock, one pip width (6) apart.
            float minx = 1e9f, maxx = -1e9f;
            for (uint32_t l = 0; l < 13; ++l)
                for (const HudQuad& q : h3.draw_list().items(l, HudList::Alpha))
                    if (std::fabs(q.max.x - q.min.x - 6.0f) < 0.01f && std::fabs(q.max.y - q.min.y - 10.0f) < 0.01f) {
                        minx = std::min(minx, 0.5f * (q.min.x + q.max.x));
                        maxx = std::max(maxx, 0.5f * (q.min.x + q.max.x));
                    }
            CHECK(std::fabs(maxx - minx - 54.0f) < 0.01f);
            d.advanced_map_open = false;
            h3.build(d);
            CHECK(h3.stats().day_pip_anchor == GalacticHudStats::DayPipAnchor::FightBar);
            h2.build(g);
        }
        // Organiser visibility follows organize_open alone (no selected planet: still shown).
        std::vector<GalacticPlanetInfo> none = planets;
        for (GalacticPlanetInfo& p : none) p.selected = false;
        g.planets = none;
        h2.build(g);
        CHECK(h2.selected_planet() < 0);
        // OI 2026-10-28: planet icons (garrison inside, fleet stacks under the name); none on a
        // fogged planet, whose label stays.
        std::vector<GalacticPlanetInfo> icons = planets;
        for (size_t i = 0; i < 2; ++i) {
            icons[i].garrison_icon = "i_button_units.tga";
            icons[i].stacks = {{1, {}, 3, false, false}, {0, "i_button_units.tga", 1, false, false}};
        }
        icons[1].fogged = true;
        g.planets = icons;
        h2.build(g);
        CHECK(h2.stats().labels == 2 && h2.stats().garrison_icons == 1 &&
              h2.stats().stack_icons == 2);
        bool stack3 = false;
        for (const HudQuad& q : h2.draw_list().items(0, HudList::Alpha)) {  // by size: 3-stack 24 x 12 at Scale 1
            const float w = q.max.x - q.min.x, h = q.max.y - q.min.y;
            stack3 = stack3 || (std::fabs(w - 24.0f) < 0.01f && std::fabs(h - 12.0f) < 0.01f);
        }
        CHECK(stack3);
        // §1.5: one medal per faction with a surface hero, none on a fogged planet.
        const uint32_t no_medal = h2.stats().label_icons;
        icons[0].hero_rebel = icons[0].hero_empire = true;
        icons[1].hero_empire = true;
        g.planets = icons;
        h2.build(g);
        CHECK(h2.stats().label_icons == no_medal + 2);
        size_t medals = 0;
        for (const HudQuad& q : h2.draw_list().items(0, HudList::Alpha))
            if (std::fabs(q.max.x - q.min.x - 14.0f) < 0.01f && std::fabs(q.max.y - q.min.y - 14.0f) < 0.01f) ++medals;
        CHECK(medals == 2);
    }
    // Zoomed out (state 1 at z = 0.2): no labels.
    GalacticHudFrame f2 = f;
    f2.zoom_state = 1;
    f2.zoom = 0.2f;
    hud.build(f2);
    CHECK(hud.stats().labels == 0);
    hud.build(f);
    // Clicks: build pad -> Production(type 101, planet 101); queue -> cancel; fleet slot -> GalacticSelect; option -> Button.
    auto click_component = [&](std::string_view name, std::vector<commands::EventData>& cmds, std::vector<HudAction>& acts) {
        const int32_t ci = table.find(name);
        const HudHitBox* hb = nullptr;
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == ci) hb = &h;
        CHECK(hb != nullptr);
        if (!hb) return;
        const Vec2f px = hud.canvas().canvas_to_pixel(0.5f * (hb->min.x + hb->max.x), 0.5f * (hb->min.y + hb->max.y));
        CHECK(hud.hit_test(static_cast<int32_t>(px.x), static_cast<int32_t>(px.y)) == ci);
        RoutedInput ri;
        ri.hover_component = ci;
        ri.hud = {{HudPointerEvent::Kind::Press, Button::Left, ci, 0, 0}, {HudPointerEvent::Kind::Release, Button::Left, ci, 0, 0},
                  {HudPointerEvent::Kind::Click, Button::Left, ci, 0, 0}};
        hud.update_input(ri, false, false, 1.0, cmds, &acts);
    };
    std::vector<commands::EventData> cmds;
    std::vector<HudAction> acts;
    click_component("b_Create_00", cmds, acts);
    CHECK(cmds.size() == 1 && cmds[0] == commands::ev::production(101, 101));
    click_component("queue00", cmds, acts);
    CHECK(cmds.size() == 2 && cmds[1] == commands::ev::production(7, 101, true));
    click_component("q1_0002", cmds, acts);
    CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::GalacticSelect && acts[0].ids == std::vector<uint32_t>{902});
    // GALACTIC_DIALOGS §5.4: planet_info has no fallback spot (the yellow rectangle); it shows on its shell bone only.
    {
        const int32_t pi = table.find("planet_info");
        bool any = false;
        for (const HudHitBox& h : hud.hit_boxes()) any = any || h.component == pi;
        CHECK(!any);
        const auto ms = static_cast<uint32_t>(table.find("map_shell"));
        ShellBone pb;
        pb.name = "planet_info";
        pb.x = 300.0f;
        pb.half_size = {50.0f, 11.0f};
        hud.layout().set_shell_bones(ms, {pb});
        hud.layout().set_shell_active(ms, true);
        hud.build(f);
    }
    click_component("planet_info", cmds, acts);
    CHECK(acts.size() == 2 && acts[1].kind == HudAction::Kind::PlanetInfo && acts[1].unit == 101);
    click_component("b_option_g", cmds, acts);
    CHECK(acts.size() == 3 && acts[2].kind == HudAction::Kind::Button && acts[2].name == "b_option_g");
    // Production-disabled options (story DISABLE_BUILDABLE, REMAINING_GAPS §1.5): no pad by default; greyed and
    // unclickable with show_production_disabled.
    GalacticHudFrame f3 = f;
    f3.providers.build_options = [](uint32_t, std::vector<GalacticBuildOption>& out) {
        GalacticBuildOption o;
        o.type_index = 40;
        o.icon = "i_button_units.tga";
        o.production_disabled = true;
        out.push_back(o);
        o.type_index = 41;
        o.production_disabled = false;
        out.push_back(o);
    };
    hud.build(f3);
    CHECK(hud.stats().build_buttons == 1);
    cmds.clear();
    click_component("b_Create_00", cmds, acts);  // the enabled option took the first pad
    CHECK(cmds.size() == 1 && cmds[0] == commands::ev::production(41, 101));
    f3.show_production_disabled = true;
    hud.build(f3);
    CHECK(hud.stats().build_buttons == 2);
    cmds.clear();
    click_component("b_Create_00", cmds, acts);  // greyed: swallowed
    click_component("b_Create_01", cmds, acts);
    CHECK(cmds.size() == 1 && cmds[0] == commands::ev::production(41, 101));
    // QA D3: one grid. Structures first, units straight after them on the same pads, column by column; the unit pads
    // of the old second grid stay empty, and every pad has its own spot.
    GalacticHudFrame f4 = f;
    f4.providers.build_options = [](uint32_t, std::vector<GalacticBuildOption>& out) {
        GalacticBuildOption o;
        o.icon = "i_button_units.tga";
        o.type_index = 50;  // unit
        out.push_back(o);
        o.type_index = 51;
        o.structure = true;
        out.push_back(o);
        o.type_index = 52;  // unit
        o.structure = false;
        out.push_back(o);
    };
    hud.build(f4);
    CHECK(hud.stats().build_buttons == 3);
    cmds.clear();
    click_component("b_Create_00", cmds, acts);
    click_component("b_Create_01", cmds, acts);
    click_component("b_Create_02", cmds, acts);
    CHECK(cmds.size() == 3 && cmds[0] == commands::ev::production(51, 101) && cmds[1] == commands::ev::production(50, 101) &&
          cmds[2] == commands::ev::production(52, 101));
    for (const HudHitBox& h : hud.hit_boxes()) CHECK(h.component != table.find("build_pad00") && h.component != table.find("build_pad01"));
    {
        const ComponentPlacement& c0 = hud.layout().placement(static_cast<uint32_t>(table.find("b_Create_00")));
        const ComponentPlacement& c1 = hud.layout().placement(static_cast<uint32_t>(table.find("b_Create_01")));
        const ComponentPlacement& c2 = hud.layout().placement(static_cast<uint32_t>(table.find("b_Create_02")));
        CHECK(c0.center.x == c1.center.x && c0.center.y > c1.center.y && std::fabs(c2.center.x - c0.center.x - 54.0f) < 1e-3f &&
              c2.center.y == c0.center.y);
    }
}

void test_lua_gui() {
    ComponentTable table;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kGalacticXml), "g.xml");
    table.parse(doc.root());
    HudButtons b;
    b.bind(table);
    TextDatabase text;
    text.add("TEXT_HELLO", u"Hello");
    auto call = [](GuiScriptOp op, std::vector<GuiScriptArg> args) {
        GuiScriptCall c;
        c.op = op;
        c.args = std::move(args);
        return c;
    };
    const uint32_t opt = static_cast<uint32_t>(table.find("b_option_g"));
    std::vector<GuiScriptCall> calls = {
        call(GuiScriptOp::ComponentVisibility, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(false)}),
        call(GuiScriptOp::ComponentEnable, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(false)}),
        call(GuiScriptOp::ComponentText, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(std::string("TEXT_HELLO"))}),
        call(GuiScriptOp::ComponentColor, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(1.0), GuiScriptArg::of(0.5),
                                           GuiScriptArg::of(0.0), GuiScriptArg::of(1.0)}),
        call(GuiScriptOp::TextColor, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(255.0), GuiScriptArg::of(0.0),
                                      GuiScriptArg::of(0.0), GuiScriptArg::of(255.0)}),
        call(GuiScriptOp::ComponentFlash, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(true), GuiScriptArg::of(1.0),
                                           GuiScriptArg::of(1.0)}),
        call(GuiScriptOp::ComponentBlink, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(true), GuiScriptArg::of(1.0),
                                           GuiScriptArg::of(1.0)}),
        call(GuiScriptOp::ButtonIcon, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(std::string("i_icon.tga")),
                                       GuiScriptArg::of(1.0), GuiScriptArg::of(1.0), GuiScriptArg::of(1.0), GuiScriptArg::of(1.0)}),
        call(GuiScriptOp::ComponentPlayAnim, {GuiScriptArg::of(std::string("b_option_g")), GuiScriptArg::of(std::string("Idle")),
                                              GuiScriptArg::of(true), GuiScriptArg::of(1.0)}),
        call(GuiScriptOp::ComponentVisibility, {GuiScriptArg::of(std::string("no_such_component")), GuiScriptArg::of(true)}),
        call(GuiScriptOp::ComponentEnable, {GuiScriptArg::of(std::string("b_option_g"))}),
    };
    std::vector<std::string> errors;
    CHECK(apply_gui_scripts(calls, table, b, &text, 3.0, &errors) == 9 && errors.size() == 2);
    const ButtonRuntime* r = b.find_state(opt);
    CHECK(r && r->hidden && r->disabled && r->text == U"Hello" && (r->tint == Rgba8{255, 128, 0, 255}) && r->text_color_set &&
          (r->text_color == Rgba8{255, 0, 0, 255}) && r->flash && r->blink_start == 3.0 && r->icon_override == "i_icon.tga" &&
          r->anim == "Idle" && r->anim_loop);
    calls = {call(GuiScriptOp::ComponentStopFlash, {GuiScriptArg::of(std::string("b_option_g"))}),
             call(GuiScriptOp::ComponentStopAnim, {GuiScriptArg::of(std::string("b_option_g"))})};
    apply_gui_scripts(calls, table, b, &text, 4.0);
    CHECK(!r->flash && r->anim.empty());
#ifdef ALAMO_UI_HAVE_LUA
    lua::PresentationLog log;
    lua::PresentationEvent& e = log.record(12, lua::PresentationOp::GuiComponentVisibility);
    lua::PresentationArg a0, a1;
    a0.kind = lua::PresentationArg::Kind::String;
    a0.text = "b_option_g";
    a1.kind = lua::PresentationArg::Kind::Bool;
    a1.b = true;
    e.args = {a0, a1};
    log.record(13, lua::PresentationOp::FadeScreenIn);
    std::vector<GuiScriptCall> lc;
    CHECK(gui_script_calls(log, lc) == 1 && lc[0].frame == 12 && lc[0].op == GuiScriptOp::ComponentVisibility &&
          lc[0].args.size() == 2 && lc[0].args[1].b);
    CHECK(apply_gui_scripts(lc, table, b, &text, 5.0) == 1 && !r->hidden);
#else
    std::printf("lua gui bridge: alamo_lua not configured, skipped\n");
#endif
}

// [port: Gabriel playtest] image pages: when all slots are used the least recently requested one is reused (animated
// cursors alone need more than kHudImagePages textures); names the host failed to load never take a slot again.
void test_image_pages() {
    GuiImagePages pages;
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kHudImagePages; ++i) names.push_back("tex_" + std::to_string(i) + ".tga");
    for (const std::string& n : names) CHECK(pages.page(n) >= 0);
    for (GuiImagePages::Entry& e : pages.entries()) pages.set_loaded(e.page, 8, 8);
    const int32_t p0 = pages.page(names[0]);  // touch 0: 1 is now the oldest
    CHECK(pages.resolve(names[0]).valid);
    const int32_t pn = pages.page("cursor_00.tga");
    CHECK(pn >= 0 && pn != p0 && pn == kHudPageImageBase + 1);
    CHECK(!pages.resolve("cursor_00.tga").valid);  // pending upload
    CHECK(pages.entries().size() == kHudImagePages);
    for (GuiImagePages::Entry& e : pages.entries())
        if (e.width == 0) pages.set_failed(e.page);
    CHECK(pages.page("CURSOR_00.TGA") == -1);       // failed names stay out
    CHECK(pages.page("other.tga") == pn);           // and their slot is reused first
    CHECK(pages.resolve(names[0]).valid && pages.page(names[2]) == kHudPageImageBase + 2);
}

#ifdef ALAMO_UI_HAVE_VFS
void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real gui: ALAMO_FOC_DATA not set, skipped\n");
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
    std::printf("real gui: %zu dialogs, %zu controls, %zu ids, %zu skin keys, %zu override sets, %zu fonts, %zu tooltips, %zu warnings%s%s\n",
                gd.script.dialogs().size(), gd.script.control_count(), gd.ids.size(), gd.skin.default_keys(),
                gd.skin.override_sets(), gd.skin.font_entries(), gd.skin.tooltip_count(), gd.script.warnings.size(),
                err.empty() ? "" : "; ", err.c_str());
    for (size_t i = 0; i < gd.script.warnings.size() && i < 5; ++i) std::printf("  %s\n", gd.script.warnings[i].c_str());
    CHECK(gd.script.dialogs().size() == 111 && gd.script.warnings.empty());
    CHECK(gd.script.control_count() > 2400 && gd.ids.size() > 2000);
    CHECK(gd.skin.default_keys() >= 80 && gd.skin.override_sets() >= 80 && gd.skin.font_entries() >= 290);
    // Every symbolic control id resolves in Resource.h.
    size_t unresolved = 0, gadgets[16] = {};
    for (const RcDialog& d : gd.script.dialogs()) {
        if (!gd.ids.id(d.name)) ++unresolved;
        for (const RcControl& c : d.controls) {
            ++gadgets[static_cast<size_t>(c.gadget)];
            // The shipped Resource.h lacks IDC_TEXT_AUDIO_3D_TECHNOLOGY (IDD_AUDIO_OPTIONS_DIALOG): a data defect, ignored.
            if (!c.id.empty() && (c.id[0] < '0' || c.id[0] > '9') && c.id != "IDC_STATIC" && !gd.ids.id(c.id) &&
                c.id != "IDC_TEXT_AUDIO_3D_TECHNOLOGY") {
                ++unresolved;
                std::printf("  unresolved %s in %s\n", c.id.c_str(), d.name.c_str());
            }
        }
    }
    std::printf("real gui: unresolved ids %zu; gadgets:", unresolved);
    for (size_t g = 0; g < 15; ++g)
        if (gadgets[g]) std::printf(" %s %zu", gadget_type_name(static_cast<GuiGadgetType>(g)), gadgets[g]);
    std::printf("\n");
    CHECK(unresolved == 0 && gadgets[static_cast<size_t>(GuiGadgetType::Unknown)] == 0);
    CHECK(gadgets[static_cast<size_t>(GuiGadgetType::CustomImage)] == 718 && gadgets[static_cast<size_t>(GuiGadgetType::RadioButton)] == 122);
    const RcDialog* mm = gd.script.find("IDD_MAIN_MENU_DIALOG");
    CHECK(mm && mm->w == 512 && mm->h == 1024 && mm->find("IDC_MENU_PETRO_LOGO") >= 0 &&
          mm->controls[static_cast<size_t>(mm->find("IDC_MENU_PETRO_LOGO"))].x == -374);
    CHECK(gd.skin.texture("IDD_MAIN_MENU_DIALOG", "", "Frame_Background") == "MenuBack_overlay_panel.tga");
    CHECK(gd.skin.texture("IDD_LAND_BATTLE_LOAD_DIALOG", "", "Frame_Background") == "loadmenuback.tga");
    CHECK(gd.skin.font("IDD_LAND_BATTLE_LOAD_DIALOG", "IDC_STATIC", GuiGadgetType::Text, TextAlign::Left).size == 8.0f);
    // Campaigns from the data layer.
    data::GameDatabase db;
    std::vector<std::string> diag;
    db.load(*files, {}, &diag);
    const CampaignMenu menu = build_campaign_menu(db);
    std::printf("real gui: %zu campaigns -> %zu story, %zu custom, %zu tutorial sets\n", db.campaigns().size(), menu.story.size(),
                menu.custom.size(), menu.tutorials.size());
    for (const CampaignSetInfo& s : menu.custom) std::printf("  custom %s (%zu factions)\n", s.set.c_str(), s.factions.size());
    CHECK(!menu.story.empty() && menu.custom.size() >= 6 && !menu.tutorials.empty());
    bool uw = false;
    for (const CampaignSetInfo& s : menu.story)
        for (const CampaignChoice& c : s.factions) uw = uw || c.faction == "Underworld";
    CHECK(uw);
    // The real flow: main menu -> single player -> custom campaign -> start.
    TextDatabase text;
    load_master_text(*files, "ENGLISH", text);
    TextureAtlas atlas;
    auto raster = make_box_rasterizer();
    FontCache fonts(*raster);
    GuiManager gui;
    gui.init(gd.script, gd.skin, atlas, fonts, &text);
    gui.resize(1024, 768);
    FrontEnd fe;
    fe.init(gui, menu, {});
    fe.show_main_menu();
    std::vector<MenuAction> acts;
    auto clicked = [&](std::string_view dialog, std::string_view control) {
        std::vector<GuiEvent> e(1);
        e[0].kind = GuiEvent::Kind::Clicked;
        e[0].dialog = std::string(dialog);
        e[0].control = std::string(control);
        fe.update(e, InputFrame{}, acts);
    };
    clicked("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES");
    clicked("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_CUSTOM_CAMPAIGN_BUTTON");
    CHECK(fe.top() == "IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG");
    GuiDialog* st = gui.top();
    CHECK(st && st->get("IDC_STAGING_MAP_LIST")->items.size() == menu.custom.size() && st->get("IDC_FACTION_COMBO")->items.size() >= 2);
    clicked("IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG", "IDC_PLAY_BUTTON");
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::StartCampaign && db.find_campaign(acts[0].campaign) != nullptr);
    if (!acts.empty()) std::printf("real gui: start %s as %s\n", acts[0].campaign.c_str(), acts[0].faction.c_str());
    for (const char* k : {"TEXT_BUTTON_YES", "TEXT_BUTTON_NO", "TEXT_BUTTON_QUIT_GAME", "TEXT_OBJECTIVES", "TEXT_DIFFICULTY_EASY_NAME",
                          "TEXT_FACTION_EMPIRE", "TEXT_FACTION_REBEL", "TEXT_FACTION_UNDERWORLD", "TEXT_BUTTON_BEGIN"})
        std::printf("real gui: text key %s %s\n", k, text.find(k) ? "found" : "MISSING");
}
#else
void test_real_data() { std::printf("real gui: alamo_vfs not configured, skipped\n"); }
#endif

}  // namespace

int main() {
    test_parse();
    test_placement();
    test_routing_and_controls();
    test_menus();
    test_load_screen();
    test_galactic_hud();
    test_galactic_dialogs();
    test_lua_gui();
    test_image_pages();
    test_real_data();
    if (g_fail) {
        std::printf("alamo_ui_gui_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_gui_tests: all passed\n");
    return 0;
}
