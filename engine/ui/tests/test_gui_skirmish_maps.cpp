// test_gui_skirmish_maps (p4ui t06 + p4uim, p4uia, p4uis; docs/re/UI.md sections 9 and 3.7.1): the skirmish staging screen in
// FrontEnd on a fake provider: Official / Custom tabs, the Land / Space game type list filtering the rows (a first opening is
// on Land, the type selected last is remembered at once), row text ("(N) name", missing key, file-name fallback with the
// 22 + "..." cut), order (row text case-insensitively, ties by map id), the initial selection (the map selected last for the
// game type, else Kuat on Space, else row 0), the map remembered on every selection change, that Start sends the selected
// row's map id through the filtered / sorted view, the preview image with the numbered start markers, the hidden description
// box and the colour combos (items, default = slot index, the slot-order clash rule, colours carried in StartSkirmish).
// Data-free.
#include "test_gui_screens_common.hpp"

namespace test_gui_screens_t {

namespace {

constexpr const char* kSkirmishRc = R"RC(
IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG DIALOGEX 0, 0, 400, 300
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    PUSHBUTTON      "x",IDC_NEW_CAMPAIGN_BUTTON,10,10,100,20
    PUSHBUTTON      "x",IDC_CUSTOM_CAMPAIGN_BUTTON,10,40,100,20
    PUSHBUTTON      "x",IDC_SKIRMISH_GAMES_BUTTON,10,70,100,20
    PUSHBUTTON      "x",IDC_BACK_BUTTON_MAIN,10,100,100,20
END

IDD_SKIRMISH_STAGING_DIALOG DIALOGEX 0, 0, 620, 441
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    PUSHBUTTON      "x",IDC_CANCEL_BUTTON,19,398,190,24
    PUSHBUTTON      "x",IDC_SKIRMISH_START_GAME_BUTTON,405,398,190,24
    PUSHBUTTON      "x",IDC_SELECT_OFFICIAL_MAPS,18,40,100,20
    PUSHBUTTON      "x",IDC_SELECT_CUSTOM_MAPS,130,40,100,20
    LISTBOX         IDC_STAGING_MAP_LIST,18,68,380,327,LBS_SORT | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    LISTBOX         IDC_GAME_TYPE_LIST,240,10,150,40,LBS_SORT | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    CONTROL         "Custom2",IDC_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,410,68,100,100
    LTEXT           "",IDC_SCENARIO_DESCRIPTION_TEXT_BOX,410,180,100,80
    COMBOBOX        IDC_PLAYER_COLOR_0,520,68,75,216,CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_COLOR_1,520,100,75,216,CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP
    COMBOBOX        IDC_PLAYER_COLOR_2,520,132,75,216,CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP
END
)RC";

constexpr const char* kSingle = "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG";
constexpr const char* kStaging = "IDD_SKIRMISH_STAGING_DIALOG";

SkirmishMapInfo map_row(std::string id, std::string key, uint32_t players, bool space, bool custom, std::u32string label = {}) {
    SkirmishMapInfo m;
    m.map = std::move(id);
    m.name_key = std::move(key);
    m.players = players;
    m.max_players = players ? players : 2;
    m.space = space;
    m.custom = custom;
    m.label = std::move(label);
    return m;
}

std::vector<std::u32string> items_of(const GuiManager& gui) {
    const GuiDialog* d = gui.find(kStaging);
    const GuiControl* l = d ? d->get("IDC_STAGING_MAP_LIST") : nullptr;
    return l ? l->items : std::vector<std::u32string>{};
}
int32_t selected_of(const GuiManager& gui) {
    const GuiDialog* d = gui.find(kStaging);
    const GuiControl* l = d ? d->get("IDC_STAGING_MAP_LIST") : nullptr;
    return l ? l->selected : -2;
}

// A 9-colour palette whose entry i is (10 * (i + 1), 0, 0).
std::vector<Rgba8> palette9() {
    std::vector<Rgba8> p;
    for (uint8_t i = 0; i < 9; ++i) p.push_back({static_cast<uint8_t>(10 * (i + 1)), 0, 0, 255});
    return p;
}

struct Rig {
    Env env;
    FrontEnd fe;
    std::vector<SkirmishMapInfo> maps;
    std::vector<MenuAction> acts;
    std::vector<std::string> previews_asked;  // map ids the preview provider was called with
    explicit Rig(std::vector<SkirmishMapInfo> m, bool palette = true, bool preview = true) : maps(std::move(m)) {
        env.script.merge(kSkirmishRc);
        env.text.add("TEXT_MAP_NAME_KUAT", u"Kuat");
        env.text.add("TEXT_MAP_NAME_ALPHA", u"Alpha");
        env.text.add("TEXT_MAP_NAME_ENDOR", u"Endor");
        env.text.add("TEXT_GAME_TYPE_TEAM_LAND", u"Ground Battle");
        env.text.add("TEXT_GAME_TYPE_TEAM_SPACE", u"Space Battle");
        env.text.add("TEXT_GAME_DESCRIPTION_TEAM_LAND", u"Land desc");
        env.text.add("TEXT_GAME_DESCRIPTION_TEAM_SPACE", u"Space desc");
        MenuProviders p;
        p.skirmish_maps = [this] { return maps; };
        if (palette) p.player_colors = palette9;
        if (preview)
            p.skirmish_preview = [this](const SkirmishMapInfo& m) {
                previews_asked.push_back(m.map);
                SkirmishPreview pv;
                pv.texture = "tex:" + m.map;
                pv.starts = {{0.25f, 0.5f}, {0.75f, 0.5f}, {0.5f, 0.1f}};
                return pv;
            };
        fe.init(env.gui, {}, p);
    }
    void game_type(bool space) {
        env.gui.find(kStaging)->set_selected("IDC_GAME_TYPE_LIST", space ? 1 : 0);
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, kStaging, "IDC_GAME_TYPE_LIST", space ? 1 : 0)}, InputFrame{}, acts);
    }
    void colour(int32_t slot, int32_t value) {
        const std::string id = "IDC_PLAYER_COLOR_" + std::to_string(slot);
        env.gui.find(kStaging)->set_selected(id, value);
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, kStaging, id, value)}, InputFrame{}, acts);
    }
    const GuiControl* control(const char* id) const { return env.gui.find(kStaging)->get(id); }
    void click(const char* dialog, const char* control) {
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, dialog, control)}, InputFrame{}, acts);
    }
    void open_staging() {
        env.gui.close_all();
        env.gui.open(kSingle);
        click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    }
    void select(int32_t row) {
        env.gui.find(kStaging)->set_selected("IDC_STAGING_MAP_LIST", row);
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, kStaging, "IDC_STAGING_MAP_LIST", row)}, InputFrame{}, acts);
    }
    // Start; returns the map id sent ("" = nothing sent).
    std::string start() {
        acts.clear();
        click(kStaging, "IDC_SKIRMISH_START_GAME_BUTTON");
        return acts.size() == 1 && acts[0].kind == MenuAction::Kind::StartSkirmish ? acts[0].map : std::string();
    }
};

}  // namespace

void test_skirmish_maps() {
    const std::string kLong = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 26 characters: cut to 22 + "..."
    const std::string k25 = "A234567890123456789012345";   // 25 characters: kept whole
    Rig rig({
        map_row("_MP_LAND_ENDOR", "TEXT_MAP_NAME_ENDOR", 4, false, false),
        map_row("_MP_LAND_ALPHA", "TEXT_MAP_NAME_ALPHA", 2, false, false),
        map_row("_MP_SPACE_KUAT", "TEXT_MAP_NAME_KUAT", 4, true, false),
        map_row("_MP_SPACE_ABC", "TEXT_NO_SUCH_KEY", 2, true, false),  // a key without text: the raw key
        map_row("DATA\\CUSTOMMAPS\\My Very Long Custom Map Name.TED", "", 2, false, true),
        map_row("SHORT_NAME", "", 2, false, true),
        map_row("Data/Art/Maps/Short_Two.ted", "", 2, false, true),  // directory and extension dropped
        map_row("DATA\\CUSTOMMAPS\\CSPACE.TED", "", 2, true, true),
        map_row("_MP_LAND_LABELED", "", 2, false, false, U"Zed Label"),  // a provider label wins
        map_row(kLong, "", 2, false, false),
        map_row(k25, "", 2, false, false),
        map_row("_MP_LAND_TWIN_B", "", 2, false, false, U"Twin"),  // equal row text: ordered by map id
        map_row("_MP_LAND_TWIN_A", "", 2, false, false, U"Twin"),
    });
    rig.open_staging();
    CHECK(rig.fe.top() == kStaging);
    const GuiDialog* d = rig.env.gui.find(kStaging);
    CHECK(d && d->get("IDC_SELECT_OFFICIAL_MAPS")->visible && d->get("IDC_SELECT_CUSTOM_MAPS")->visible);

    // The game type list (t01): Land, Space from the text database; a first opening is on Land (UI.md 9.7).
    CHECK(rig.control("IDC_GAME_TYPE_LIST")->items == (std::vector<std::u32string>{U"Ground Battle", U"Space Battle"}));
    CHECK(!rig.fe.skirmish_space_type() && rig.control("IDC_GAME_TYPE_LIST")->selected == 0);

    // Official tab on open, Land type: the land maps by row text; "(N) " + the localised name.
    const std::vector<std::u32string> official_space = {U"(2) TEXT_NO_SUCH_KEY", U"(4) Kuat"};
    const std::vector<std::u32string> official_land = {U"(2) Alpha", U"(4) Endor", U"A234567890123456789012345",
                                                       U"ABCDEFGHIJKLMNOPQRSTUV...", U"Twin", U"Twin", U"Zed Label"};
    CHECK(!rig.fe.skirmish_custom_tab());
    CHECK(items_of(rig.env.gui) == official_land);
    // Initial selection: nothing remembered for Land, so row 0.
    CHECK(selected_of(rig.env.gui) == 0);
    CHECK(rig.fe.skirmish_map_row("_MP_LAND_ENDOR") == 1 && rig.fe.skirmish_map_row("_MP_SPACE_KUAT") == -1 && rig.fe.skirmish_map_row(kLong) == 3);
    CHECK(rig.start() == "_MP_LAND_ALPHA");
    // Every row maps back to its own map id (row order is not the provider order), through the sorted view.
    const char* const ids[7] = {"_MP_LAND_ALPHA", "_MP_LAND_ENDOR", "A234567890123456789012345", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "_MP_LAND_TWIN_A",
                                "_MP_LAND_TWIN_B", "_MP_LAND_LABELED"};
    for (int32_t r = 0; r < 7; ++r) {
        rig.select(r);
        CHECK(rig.start() == ids[r]);
    }
    // The same event again changes nothing (no refill: the selection stays on the last row).
    rig.game_type(false);
    CHECK(selected_of(rig.env.gui) == 6);

    // Space type: only the space maps; nothing remembered for Space yet, so row 0 (UI.md 9.7; modc t19: no map by name).
    rig.game_type(true);
    CHECK(rig.fe.skirmish_space_type() && items_of(rig.env.gui) == official_space && selected_of(rig.env.gui) == 0);
    CHECK(rig.fe.skirmish_map_row("_mp_space_kuat") == 1 && rig.fe.skirmish_map_row("_MP_LAND_ENDOR") == -1 && rig.fe.skirmish_map_row("nope") == -1);
    CHECK(rig.start() == "_MP_SPACE_ABC");

    // UI.md 9.5: the map follows the list selection, not Start. Pick a row on each type without starting; after switching away
    // and back the row picked is selected again.
    rig.select(0);  // Space: ABC
    rig.game_type(false);
    CHECK(items_of(rig.env.gui) == official_land && selected_of(rig.env.gui) == 6);
    rig.select(4);  // Land: Twin A
    rig.game_type(true);
    CHECK(items_of(rig.env.gui) == official_space && selected_of(rig.env.gui) == 0);
    rig.game_type(false);
    CHECK(selected_of(rig.env.gui) == 4 && rig.start() == "_MP_LAND_TWIN_A");

    // Custom tab (Land type): a click switches and refills; only the custom land maps; the file name fallback. The remembered
    // Land map is an official one, not listed here: row 0, which becomes the remembered map.
    rig.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(rig.fe.skirmish_custom_tab() && !rig.fe.skirmish_space_type());
    const std::vector<std::u32string> custom_land = {U"My Very Long Custom Ma...", U"SHORT_NAME", U"Short_Two"};
    CHECK(items_of(rig.env.gui) == custom_land);
    CHECK(selected_of(rig.env.gui) == 0);
    CHECK(rig.start() == "DATA\\CUSTOMMAPS\\My Very Long Custom Map Name.TED");
    rig.select(2);
    CHECK(rig.start() == "Data/Art/Maps/Short_Two.ted");
    CHECK(rig.fe.skirmish_map_row("Data/Art/Maps/Short_Two.ted") == 2 && rig.fe.skirmish_map_row("_MP_LAND_ENDOR") == -1);
    // Space type on the Custom tab: the one custom space map.
    rig.game_type(true);
    CHECK(items_of(rig.env.gui) == (std::vector<std::u32string>{U"CSPACE"}) && selected_of(rig.env.gui) == 0);
    CHECK(rig.start() == "DATA\\CUSTOMMAPS\\CSPACE.TED");
    // Back to Official: Space's remembered map is the custom one, not listed here, so row 0 (which is remembered then).
    rig.click(kStaging, "IDC_SELECT_OFFICIAL_MAPS");
    CHECK(!rig.fe.skirmish_custom_tab() && items_of(rig.env.gui) == official_space && selected_of(rig.env.gui) == 0);

    // Closing and reopening the dialog: the Official tab again, on the game type selected last (Space), with its remembered map.
    rig.click(kStaging, "IDC_CANCEL_BUTTON");
    CHECK(rig.fe.top() == kSingle);
    rig.click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    CHECK(rig.fe.top() == kStaging && !rig.fe.skirmish_custom_tab() && rig.fe.skirmish_space_type() && items_of(rig.env.gui) == official_space &&
          selected_of(rig.env.gui) == 0);
    CHECK(rig.control("IDC_GAME_TYPE_LIST")->selected == 1);
    rig.select(0);
    // The type is remembered when it is selected, not when a game is started: switch to Land and leave without Start.
    rig.game_type(false);
    CHECK(items_of(rig.env.gui) == official_land && selected_of(rig.env.gui) == 0);  // Land's remembered map was a custom one: row 0
    rig.select(6);
    rig.click(kStaging, "IDC_CANCEL_BUTTON");
    rig.click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    CHECK(!rig.fe.skirmish_space_type() && selected_of(rig.env.gui) == 6 && rig.control("IDC_GAME_TYPE_LIST")->selected == 0);  // Land again
    CHECK(rig.start() == "_MP_LAND_LABELED");
    rig.game_type(true);
    CHECK(selected_of(rig.env.gui) == 0);  // Space's remembered ABC

    // A fresh list opens on Land (no official land maps here); switching to Space without Kuat gives row 0; custom maps are not
    // shown on the Official tab and vice versa.
    Rig plain({map_row("_MP_SPACE_B", "", 2, true, false), map_row("_MP_SPACE_A", "", 2, true, false), map_row("MY_CUSTOM", "", 2, false, true)});
    plain.open_staging();
    CHECK(!plain.fe.skirmish_space_type() && items_of(plain.env.gui).empty() && selected_of(plain.env.gui) == -1 && plain.start().empty());
    plain.game_type(true);
    CHECK(items_of(plain.env.gui) == (std::vector<std::u32string>{U"_MP_SPACE_A", U"_MP_SPACE_B"}) && selected_of(plain.env.gui) == 0);
    CHECK(plain.start() == "_MP_SPACE_A");
    plain.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(items_of(plain.env.gui).empty() && selected_of(plain.env.gui) == -1 && plain.start().empty());  // the custom map is a land map
    plain.game_type(false);
    CHECK(items_of(plain.env.gui) == (std::vector<std::u32string>{U"MY_CUSTOM"}) && plain.start() == "MY_CUSTOM");

    // A tab / type without maps: empty list, nothing selected, Start sends nothing, no preview, no description.
    Rig none({map_row("_MP_LAND_ONLY", "", 2, false, false)});
    none.open_staging();
    CHECK(items_of(none.env.gui).size() == 1 && selected_of(none.env.gui) == 0 && none.previews_asked.size() == 1);
    none.previews_asked.clear();
    none.game_type(true);
    CHECK(items_of(none.env.gui).empty() && selected_of(none.env.gui) == -1 && none.start().empty());
    CHECK(none.control("IDC_IMAGE")->image.empty() && none.control("IDC_IMAGE")->markers.empty() &&
          none.control("IDC_SCENARIO_DESCRIPTION_TEXT_BOX")->text.empty() && none.previews_asked.empty());
    Rig empty(std::vector<SkirmishMapInfo>{});
    empty.open_staging();
    CHECK(items_of(empty.env.gui).empty() && selected_of(empty.env.gui) == -1 && empty.start().empty());
}

// t03 / t04 / p4uis: IDC_IMAGE follows the selected row (texture from the host, the start positions as the white numbers 1..N),
// the description box is hidden and empty, and nothing is asked of the host while no row is selected.
void test_skirmish_preview() {
    Rig rig({map_row("_MP_SPACE_KUAT", "TEXT_MAP_NAME_KUAT", 4, true, false), map_row("_MP_SPACE_ABC", "", 2, true, false),
             map_row("_MP_LAND_ALPHA", "TEXT_MAP_NAME_ALPHA", 2, false, false)});
    rig.open_staging();
    const GuiControl* img = rig.control("IDC_IMAGE");
    CHECK(img->image == "tex:_MP_LAND_ALPHA" && img->markers.size() == 3);  // the dialog opens on Land
    CHECK(img->markers[0].pos.x == 0.25f && img->markers[0].pos.y == 0.5f && img->markers[1].pos.x == 0.75f);
    // Start i shows the number i + 1 in opaque white, whatever the slot colours are (UI.md 9.6.1).
    const Rgba8 white{255, 255, 255, 255};
    CHECK(img->markers[0].label == U"1" && img->markers[1].label == U"2" && img->markers[2].label == U"3");
    CHECK(img->markers[0].color == white && img->markers[1].color == white && img->markers[2].color == white);
    // The description box is switched off and never filled (UI.md 9.6.2).
    const GuiControl* desc = rig.control("IDC_SCENARIO_DESCRIPTION_TEXT_BOX");
    CHECK(!desc->visible && desc->text.empty());
    rig.game_type(true);
    CHECK(img->image == "tex:_MP_SPACE_KUAT" && !desc->visible && desc->text.empty());
    rig.select(1);
    CHECK(img->image == "tex:_MP_SPACE_ABC" && !desc->visible && desc->text.empty());
    // A colour change leaves the numbers white.
    rig.colour(0, 4);
    CHECK(img->markers.size() == 3 && img->markers[0].color == white && img->markers[1].color == white);
    // The Land type: its row.
    rig.game_type(false);
    CHECK(img->image == "tex:_MP_LAND_ALPHA" && !desc->visible && desc->text.empty());
    // A tab without rows clears them.
    rig.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(img->image.empty() && img->markers.empty() && !desc->visible && desc->text.empty());
    // Without a preview provider the image stays empty.
    Rig bare({map_row("_MP_SPACE_KUAT", "", 2, true, false)}, true, false);
    bare.open_staging();
    bare.game_type(true);
    CHECK(bare.control("IDC_IMAGE")->image.empty() && bare.control("IDC_IMAGE")->markers.empty() &&
          !bare.control("IDC_SCENARIO_DESCRIPTION_TEXT_BOX")->visible && bare.control("IDC_SCENARIO_DESCRIPTION_TEXT_BOX")->text.empty());
}

// p4uia t01 / t02, p4uis t03: IDC_PLAYER_COLOR_n of the used slots hold the 9 palette entries as U+25A0 glyphs, the default colour
// is the slot index, the colour clash rule runs in slot order (UI.md 9.7), and StartSkirmish carries the chosen indices.
void test_skirmish_colours() {
    Rig rig({map_row("_MP_LAND_X", "", 2, false, false)});
    rig.open_staging();
    for (int32_t slot = 0; slot < 2; ++slot) {
        const GuiControl* c = rig.control(slot == 0 ? "IDC_PLAYER_COLOR_0" : "IDC_PLAYER_COLOR_1");
        CHECK(c->visible && c->items == std::vector<std::u32string>(9, std::u32string(1, 0x25A0)));
        CHECK(c->item_colors == palette9() && c->selected == slot && rig.fe.skirmish_colour(slot) == slot);
    }
    CHECK(!rig.control("IDC_PLAYER_COLOR_2")->visible && rig.fe.skirmish_colour(2) == -1);
    auto colours = [&] {
        rig.acts.clear();
        rig.click(kStaging, "IDC_SKIRMISH_START_GAME_BUTTON");
        return rig.acts.size() == 1 ? rig.acts[0].colors : std::vector<int32_t>{-1};
    };
    // The combos show what the rule leaves, and Start carries it.
    auto shown = [&](int32_t a, int32_t b) {
        return rig.fe.skirmish_colour(0) == a && rig.fe.skirmish_colour(1) == b && rig.control("IDC_PLAYER_COLOR_0")->selected == a &&
               rig.control("IDC_PLAYER_COLOR_1")->selected == b && colours() == (std::vector<int32_t>{a, b});
    };
    CHECK(shown(0, 1));
    rig.colour(1, 7);  // a free colour
    CHECK(shown(0, 7));
    rig.colour(0, 7);  // slot 1's colour: slot 0 wins, slot 1 takes the lowest colour slot 0 does not use
    CHECK(shown(7, 0));
    // Reopening resets to the slot indices.
    rig.click(kStaging, "IDC_CANCEL_BUTTON");
    rig.click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    CHECK(shown(0, 1));
    rig.colour(1, 0);  // (0,1): slot 1 picks slot 0's colour and moves to 1 again
    CHECK(shown(0, 1));
    rig.colour(0, 1);  // (0,1): slot 0 picks 1: slot 1 becomes 0
    CHECK(shown(1, 0));
    rig.click(kStaging, "IDC_CANCEL_BUTTON");
    rig.click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    rig.colour(0, 3);
    CHECK(shown(3, 1));
    rig.colour(0, 1);  // (3,1): slot 0 picks 1: slot 1 becomes 0, not 3
    CHECK(shown(1, 0));
    // Without a palette the combos stay hidden; Start still sends the default indices.
    Rig bare({map_row("_MP_LAND_X", "", 2, false, false)}, false);
    bare.open_staging();
    CHECK(!bare.control("IDC_PLAYER_COLOR_0")->visible && !bare.control("IDC_PLAYER_COLOR_1")->visible && bare.fe.skirmish_colour(0) == -1);
    bare.acts.clear();
    bare.click(kStaging, "IDC_SKIRMISH_START_GAME_BUTTON");
    CHECK(bare.acts.size() == 1 && bare.acts[0].colors == (std::vector<int32_t>{0, 1}));
}

}  // namespace test_gui_screens_t
