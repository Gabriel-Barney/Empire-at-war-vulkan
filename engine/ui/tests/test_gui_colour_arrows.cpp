// alamo_ui_colour_arrows_tests (p4ui t03, CPU): colour combos draw the item text in the item colour (docs/re/UI.md
// §3.7.1: no filled box, glyph through the control font, centred / left-justified field text, trailing characters
// dropped when too wide), the lobby's one-glyph colour items, the tech-tree arrow art, icon tints, the battle-end
// arrows and the fleet summary dividers (§3.7.2).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "alamo/data/xml.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_info_screens.hpp"
#include "alamo/ui/gui_lobby.hpp"
#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/gui_screens.hpp"
#include "alamo/ui/hud_font.hpp"

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

// Trimmed copies of the shipped dialogs (same ids), plus two plain test combos in the LAN lobby.
const char* kRc = R"RC(
IDD_LAN_LOBBY_DIALOG DIALOGEX 0, 0, 955, 701
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    COMBOBOX        IDC_LAN_COLOR_SELECT,856,53,73,169,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_TEST_COMBO_A,100,53,80,169,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_TEST_COMBO_B,300,53,160,169,CBS_DROPDOWNLIST | WS_TABSTOP
    LISTBOX         IDC_PLAYER_LIST,721,130,234,402,LBS_NOINTEGRALHEIGHT | WS_VSCROLL
END

IDD_LAN_STAGING_DIALOG DIALOGEX 0, 0, 955, 701
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    LISTBOX         IDC_STAGING_MAP_LIST,221,140,232,197,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_NAME_0,495,68,155,128,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_FACTION_0,654,68,115,120,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_COLOR_0,773,68,75,216,CBS_DROPDOWNLIST | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_TEAM_0,854,68,95,192,CBS_DROPDOWNLIST | WS_TABSTOP
END

IDD_BATTLE_END_DIALOG DIALOGEX 0, 0, 913, 667
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "Continue",IDC_CONTINUE_BUTTON,522,609,321,24
    LTEXT           "Win lose",IDC_TITLE_STATIC,36,34,838,17
    CONTROL         "",IDC_RIGHT_ARROW1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,51,246,255,14
    CONTROL         "",IDC_RIGHT_ARROW2,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,497,246,255,14
END

IDD_TECH_TREE_EMPIRE DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "",IDC_RIGHT_ARROW0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,100,150,30,14
    CONTROL         "",IDC_RIGHT_ARROW21,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,20,300,845,14
    CONTROL         "",IDC_RIGHT_ARROW99,"PETROGLYPH_DIALOG_IMAGE",NOT WS_VISIBLE | WS_TABSTOP,20,320,30,14
    CONTROL         "",IDC_DRIGHT_ARROW3,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,150,150,30,14
    CONTROL         "",IDC_RRIGHT_ARROW5,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,200,150,30,14
    CONTROL         "",IDC_DOWN_ARROW7,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,250,150,14,30
    CONTROL         "",IDC_DOWN_ARROW8,"PETROGLYPH_DIALOG_IMAGE",NOT WS_VISIBLE | WS_TABSTOP,280,150,14,30
    CONTROL         "",IDC_TECHTREE_Locked_Unit,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,100,100,40,40
    CONTROL         "",IDC_TECHTREE_Available_Unit,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,150,100,40,40
    CONTROL         "",IDC_TECHTREE_Researched_Unit,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,200,100,40,40
END

IDD_SUMMARY_SCREEN_FLEETS DIALOGEX 0, 0, 884, 606
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
FONT 8, "MS Sans Serif", 0, 0, 0x0
BEGIN
    CONTROL         "Custom1",IDC_PLANET_ICON0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,30,90,30,30
    LTEXT           "planet name0",IDC_PLANET_NAME0,64,95,100,10
    CONTROL         " ",IDC_HEROES_DIVIDERS0,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,20,130,824,14
    CONTROL         "Custom1",IDC_PLANET_ICON1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,30,150,30,30
    LTEXT           "planet name1",IDC_PLANET_NAME1,64,155,100,10
    CONTROL         " ",IDC_HEROES_DIVIDERS1,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,20,190,824,14
    CONTROL         "Custom1",IDC_PLANET_ICON2,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,30,210,30,30
    LTEXT           "planet name2",IDC_PLANET_NAME2,64,215,100,10
END
)RC";

// Per-control fonts as in GUIDialogs.xml: the colour combos use a big Arial Unicode MS glyph squashed by Stretch_Factor.
const char* kSkinXml = R"(<GUIDialogs><Textures File="MT_CommandBar"><Default>
  <Frame_Background>i_dialogue_00.tga</Frame_Background>
</Default></Textures><Fonts><Default><Global_Default><Name>Arial</Name><Size>8</Size></Global_Default></Default>
  <IDC_PLAYER_COLOR_0><Name>Arial Unicode MS</Name><Character_Padding>0</Character_Padding><Stretch_Factor>.25</Stretch_Factor>
    <Size>35</Size><Bottom_Color>33,37,47,255</Bottom_Color><Top_Color>33,37,47,255</Top_Color><Emboss>No</Emboss><Outline>No</Outline></IDC_PLAYER_COLOR_0>
  <IDC_LAN_COLOR_SELECT><Name>Arial Unicode MS</Name><Character_Padding>0</Character_Padding><Stretch_Factor>.20</Stretch_Factor>
    <Size>30</Size><Bottom_Color>33,37,47,255</Bottom_Color><Top_Color>33,37,47,255</Top_Color><Emboss>No</Emboss><Outline>No</Outline></IDC_LAN_COLOR_SELECT>
</Fonts></GUIDialogs>)";

constexpr Rgba8 kWhite{255, 255, 255, 255};
constexpr Rgba8 kRed{210, 20, 30, 255}, kGreen{12, 200, 77, 255}, kBlue{30, 60, 230, 255};

struct Fix {
    DialogScript script;
    GuiSkin skin;
    TextureAtlas atlas;
    TextDatabase text;
    std::unique_ptr<GlyphRasterizer> raster = make_box_rasterizer();
    FontCache fonts{*raster};
    GuiManager gui;
    double now = 10.0;
    Fix() {
        std::string err;
        script.parse(kRc, &err);
        const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kSkinXml), "GUIDialogs.xml");
        skin.parse(doc.root());
        atlas.texture_width = atlas.texture_height = 256;
        atlas.add({"i_dialogue_00.tga", 0, 0, 16, 16, true});
        gui.init(script, skin, atlas, fonts, &text);
        gui.resize(1024, 768);
    }
    HudDrawList draw() {
        HudDrawList dl;
        gui.build(dl, now);
        return dl;
    }
};

std::vector<HudQuad> list_quads(const HudDrawList& dl, HudList l) {
    std::vector<HudQuad> out;
    for (uint32_t layer = 0; layer < kHudLayers; ++layer)
        for (const HudQuad& q : dl.items(layer, l)) out.push_back(q);
    return out;
}
struct Bounds {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    size_t n = 0;
};
Bounds bounds(const std::vector<HudQuad>& qs) {
    Bounds b;
    for (const HudQuad& q : qs) {
        b.x0 = std::min(b.x0, q.min.x);
        b.x1 = std::max(b.x1, q.max.x);
        b.y0 = std::min(b.y0, q.min.y);
        b.y1 = std::max(b.y1, q.max.y);
        ++b.n;
    }
    return b;
}
bool any_colour(const std::vector<HudQuad>& qs, Rgba8 c) {
    for (const HudQuad& q : qs)
        if (q.color == c) return true;
    return false;
}
bool all_colour(const std::vector<HudQuad>& qs, Rgba8 c) {
    for (const HudQuad& q : qs)
        if (!(q.color == c) || q.gradient) return false;
    return !qs.empty();
}
// The closed combo field the draw code uses (no pop-down texture in the test atlas -> button as wide as the field is tall).
GuiRect field_of(const GuiDialog& d, std::string_view id, float* button_width) {
    GuiRect r = d.control_rect(static_cast<size_t>(d.find(id)));
    r.min.y = r.max.y - std::min(r.height(), 15.0f);
    if (button_width) *button_width = r.height();
    return r;
}
// Closes every combo but the one under test so only its text is on screen.
void clear_other_combos(GuiDialog& d, std::string_view keep) {
    for (const char* id : {"IDC_LAN_COLOR_SELECT", "IDC_TEST_COMBO_A", "IDC_TEST_COMBO_B"})
        if (!(std::string_view(id) == keep)) d.set_items(id, {}, -1);
}

void test_colour_combo_text() {
    Fix f;
    GuiDialog* d = f.gui.open("IDD_LAN_LOBBY_DIALOG");
    CHECK(d != nullptr);
    if (!d) return;
    clear_other_combos(*d, "IDC_LAN_COLOR_SELECT");
    GuiControl* c = d->get("IDC_LAN_COLOR_SELECT");
    c->items = {U"█", U"█", U"█"};
    c->item_colors = {kRed, kGreen, kBlue};
    c->selected = 1;
    {  // closed field: one glyph in the item colour (flat), no filled box, a flat wide bar through the per-control font
        const HudDrawList dl = f.draw();
        const std::vector<HudQuad> text = list_quads(dl, HudList::Text);
        CHECK(text.size() == 1 && all_colour(text, kGreen));
        for (Rgba8 col : {kRed, kGreen, kBlue}) CHECK(!any_colour(list_quads(dl, HudList::Alpha), col));
        if (!text.empty()) {  // Arial Unicode MS 30, Stretch_Factor 0.2: height scaled, width kept
            const float w = text[0].max.x - text[0].min.x, h = text[0].max.y - text[0].min.y;
            CHECK(w > 2.0f * h && h > 0.0f);
        }
    }
    {  // a disabled combo keeps its item colour
        c->enabled = false;
        const std::vector<HudQuad> text = list_quads(f.draw(), HudList::Text);
        CHECK(text.size() == 1 && all_colour(text, kGreen));
        c->enabled = true;
    }
    {  // open: every drop-down row is a glyph in its own colour
        c->open = true;
        const HudDrawList dl = f.draw();
        const std::vector<HudQuad> text = list_quads(dl, HudList::Text);
        CHECK(text.size() == 4 && any_colour(text, kRed) && any_colour(text, kGreen) && any_colour(text, kBlue));
        for (Rgba8 col : {kRed, kGreen, kBlue}) CHECK(!any_colour(list_quads(dl, HudList::Alpha), col));
        c->open = false;
    }
    {  // the staging combo's font (Arial Unicode MS 35 at 0.25) squashes more than the LAN one
        f.gui.close("IDD_LAN_LOBBY_DIALOG");
        GuiDialog* s = f.gui.open("IDD_LAN_STAGING_DIALOG");
        GuiControl* pc = s ? s->get("IDC_PLAYER_COLOR_0") : nullptr;
        CHECK(pc != nullptr);
        if (pc) {
            for (const char* id : {"IDC_PLAYER_NAME_0", "IDC_PLAYER_FACTION_0", "IDC_PLAYER_TEAM_0"}) s->set_items(id, {}, -1);
            pc->items = {U"■", U"■"};
            pc->item_colors = {kRed, kBlue};
            pc->selected = 0;
            const std::vector<HudQuad> text = list_quads(f.draw(), HudList::Text);
            CHECK(text.size() == 1 && all_colour(text, kRed));
            if (!text.empty()) CHECK((text[0].max.x - text[0].min.x) > 2.0f * (text[0].max.y - text[0].min.y));
        }
    }
}

void test_list_box_colours() {
    Fix f;
    GuiDialog* d = f.gui.open("IDD_LAN_LOBBY_DIALOG");
    if (!d) {
        CHECK(false);
        return;
    }
    clear_other_combos(*d, "");
    GuiControl* l = d->get("IDC_PLAYER_LIST");
    l->items = {U"AAA", U"BBB"};
    l->item_colors = {kRed, kBlue};
    const HudDrawList dl = f.draw();
    const std::vector<HudQuad> text = list_quads(dl, HudList::Text);
    CHECK(text.size() == 6 && any_colour(text, kRed) && any_colour(text, kBlue));
    for (Rgba8 col : {kRed, kBlue}) CHECK(!any_colour(list_quads(dl, HudList::Alpha), col));
}

void test_combo_field_text() {
    Fix f;
    GuiDialog* d = f.gui.open("IDD_LAN_LOBBY_DIALOG");
    if (!d) {
        CHECK(false);
        return;
    }
    clear_other_combos(*d, "IDC_TEST_COMBO_B");
    GuiControl* c = d->get("IDC_TEST_COMBO_B");
    c->items = {U"Hello"};
    c->selected = 0;
    float bw = 0.0f;
    const GuiRect fr = field_of(*d, "IDC_TEST_COMBO_B", &bw);
    {  // centred in the field left of the pop-down button, vertically inside the field
        const Bounds b = bounds(list_quads(f.draw(), HudList::Text));
        CHECK(b.n == 5);
        const float want = 0.5f * (fr.min.x + fr.max.x - bw), got = 0.5f * (b.x0 + b.x1);
        CHECK(std::fabs(got - want) < 3.0f);
        CHECK(b.y0 >= fr.min.y - 1.0f && b.y1 <= fr.max.y + 1.0f);
        CHECK(b.x0 > fr.min.x + 20.0f);
    }
    {  // left-justified variant: the field's left edge plus the 5-unit inset
        c->left_justified = true;
        const Bounds b = bounds(list_quads(f.draw(), HudList::Text));
        CHECK(b.n == 5 && std::fabs(b.x0 - (fr.min.x + 5.0f)) < 0.75f);
        c->left_justified = false;
    }
    {  // too wide: trailing characters are dropped until it fits, nothing is added
        clear_other_combos(*d, "IDC_TEST_COMBO_A");
        GuiControl* a = d->get("IDC_TEST_COMBO_A");
        a->items = {U"ABCDEFGHIJKLMNOPQRST"};
        a->selected = 0;
        float abw = 0.0f;
        const GuiRect ar = field_of(*d, "IDC_TEST_COMBO_A", &abw);
        const Bounds b = bounds(list_quads(f.draw(), HudList::Text));
        CHECK(b.n >= 1 && b.n < 20);
        CHECK(b.x1 <= ar.max.x - abw && b.x0 >= ar.min.x);
        a->items = {U"AB"};  // short text is untouched
        CHECK(bounds(list_quads(f.draw(), HudList::Text)).n == 2);
        a->items = {U"ABCDEFGHIJKLMNOPQRST"};
        a->left_justified = true;  // truncation also applies to the left-justified variant
        const Bounds lb = bounds(list_quads(f.draw(), HudList::Text));
        CHECK(lb.n >= 1 && lb.n < 20 && lb.x1 <= ar.max.x - abw);
    }
}

void test_lobby_glyph_items() {
    Fix f;
    LobbyScreens lob;
    lob.init(f.gui);
    // Without palette values the combos keep their text items.
    StagingState st;
    st.host = true;
    st.colors = {U"Red", U"Blue", U"Green"};
    st.factions = {U"Rebel", U"Empire"};
    st.teams = {U"1"};
    st.maps = {U"Kuat"};
    st.map = 0;
    StagingSlot ai;
    ai.state = SlotState::AI;
    ai.color = 1;
    st.slots = {ai};
    CHECK(lob.show_staging(st));
    GuiDialog* s = f.gui.find(dlg::kLanStaging);
    CHECK(s && s->get("IDC_PLAYER_COLOR_0")->items.size() == 3 && s->get("IDC_PLAYER_COLOR_0")->items[0] == U"Red" &&
          s->get("IDC_PLAYER_COLOR_0")->item_colors.empty());
    // With values: one U+25A0 per palette entry, the selection stays the colour index.
    st.color_values = {kRed, kBlue, kGreen};
    CHECK(lob.show_staging(st));
    s = f.gui.find(dlg::kLanStaging);
    const GuiControl* pc = s ? s->get("IDC_PLAYER_COLOR_0") : nullptr;
    CHECK(pc && pc->items.size() == 3 && pc->items[0] == U"■" && pc->items[1] == U"■" && pc->items[2] == U"■" &&
          pc->selected == 1 && pc->item_colors.size() == 3 && pc->item_colors[1] == kBlue);
    // LAN lobby colour select: U+2588.
    lob.set_colors({U"Red", U"Blue"}, 1, {kRed, kBlue});
    lob.show_lan_lobby();
    const GuiDialog* l = f.gui.find(dlg::kLanLobby);
    const GuiControl* lc = l ? l->get("IDC_LAN_COLOR_SELECT") : nullptr;
    CHECK(lc && lc->items.size() == 2 && lc->items[0] == U"█" && lc->selected == 1 && lc->item_colors.size() == 2);
    lob.set_colors({U"Red", U"Blue", U"Green"}, 2, {kRed, kBlue, kGreen});  // refresh while open
    lc = f.gui.find(dlg::kLanLobby)->get("IDC_LAN_COLOR_SELECT");
    CHECK(lc->items.size() == 3 && lc->items[2] == U"█" && lc->selected == 2);
    lob.set_colors({U"Red", U"Blue"}, 0);  // no values: text items again
    lc = f.gui.find(dlg::kLanLobby)->get("IDC_LAN_COLOR_SELECT");
    CHECK(lc->items.size() == 2 && lc->items[0] == U"Red" && lc->item_colors.empty());
}

void test_tech_tree_arrows() {
    Fix f;
    InfoScreens is;
    is.init(f.gui);
    TechTreeInfo tt;
    tt.dialog = "IDD_TECH_TREE_EMPIRE";
    tt.item = [](std::string_view type, TechTreeItem& out) {
        out.icon = "i_tech.tga";
        out.state = type == "Locked_Unit" ? TechState::Locked : type == "Researched_Unit" ? TechState::Researched : TechState::Available;
        return true;
    };
    is.set_tech_tree(tt);
    CHECK(is.show_info(InfoPage::TechTree));
    const GuiDialog* d = f.gui.find("IDD_TECH_TREE_EMPIRE");
    CHECK(d != nullptr);
    if (!d) return;
    struct Want {
        const char* id;
        const char* tex;
    };
    for (const Want& w : {Want{"IDC_RIGHT_ARROW0", "i_tree_arrow_right.tga"}, Want{"IDC_RIGHT_ARROW21", "i_tree_arrow_right.tga"},
                          Want{"IDC_DRIGHT_ARROW3", "i_tree_arrow_2right.tga"}, Want{"IDC_RRIGHT_ARROW5", "i_tree_arrow_3right.tga"},
                          Want{"IDC_DOWN_ARROW7", "i_tree_arrow_down.tga"}}) {
        const GuiControl* c = d->get(w.id);
        CHECK(c && c->image == w.tex && c->tint == kWhite && c->visible);
    }
    // invisible arrows are left alone
    CHECK(d->get("IDC_RIGHT_ARROW99")->image.empty() && d->get("IDC_DOWN_ARROW8")->image.empty());
    // icon tints: locked 90 grey, available and researched white
    CHECK(d->get("IDC_TECHTREE_Locked_Unit")->tint == (Rgba8{90, 90, 90, 255}));
    CHECK(d->get("IDC_TECHTREE_Available_Unit")->tint == kWhite);
    CHECK(d->get("IDC_TECHTREE_Researched_Unit")->tint == kWhite);
    CHECK(d->get("IDC_TECHTREE_Locked_Unit")->image == "i_tech.tga");
    // the arrow is a quad over the control rectangle in the draw list (page set by the image resolver; here none ->
    // nothing is drawn, so only the control state is asserted above).
}

void test_battle_end_arrows() {
    Fix f;
    GameScreens gs;
    gs.init(f.gui);
    BattleEndInfo be;
    be.victory = true;
    CHECK(gs.show_battle_end(be));
    const GuiDialog* d = f.gui.find(dlg::kBattleEnd);
    CHECK(d != nullptr);
    if (!d) return;
    for (const char* id : {"IDC_RIGHT_ARROW1", "IDC_RIGHT_ARROW2"}) {
        const GuiControl* c = d->get(id);
        CHECK(c && c->visible && c->image == "i_tree_arrow_right.tga" && c->tint == kWhite);
    }
}

void test_fleet_dividers() {
    Fix f;
    InfoScreens is;
    is.init(f.gui);
    SummaryInfo sm;
    sm.fleets.push_back({"i_planet.tga", U"Kuat", {}});
    sm.fleets.push_back({"i_planet.tga", U"Hoth", {}});
    is.set_summary(sm);
    CHECK(is.show_info(InfoPage::SummaryFleets));
    const GuiDialog* d = f.gui.find(dlg::kSummaryFleets);
    CHECK(d != nullptr);
    if (!d) return;
    // rows 0 and 1 filled, row 2 empty: DIVIDERS0 (above row 1) shown, DIVIDERS1 (above the empty row 2) hidden
    const GuiControl* d0 = d->get("IDC_HEROES_DIVIDERS0");
    const GuiControl* d1 = d->get("IDC_HEROES_DIVIDERS1");
    CHECK(d0 && d0->visible && d0->image == "i_tree_arrow_right.tga" && d0->tint == kWhite);
    CHECK(d1 && !d1->visible);
    CHECK(!d->get("IDC_PLANET_ICON2")->visible);
    // a third row makes the second divider appear
    sm.fleets.push_back({"i_planet.tga", U"Endor", {}});
    is.set_summary(sm);
    CHECK(is.show_info(InfoPage::SummaryFleets));
    d = f.gui.find(dlg::kSummaryFleets);
    CHECK(d && d->get("IDC_HEROES_DIVIDERS1")->visible && d->get("IDC_HEROES_DIVIDERS1")->image == "i_tree_arrow_right.tga");
}

}  // namespace

int main() {
    test_colour_combo_text();
    test_list_box_colours();
    test_combo_field_text();
    test_lobby_glyph_items();
    test_tech_tree_arrows();
    test_battle_end_arrows();
    test_fleet_dividers();
    if (g_fail) {
        std::printf("alamo_ui_colour_arrows_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_colour_arrows_tests: all passed\n");
    return 0;
}
