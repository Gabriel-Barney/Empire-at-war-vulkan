// test_gui_skirmish_maps (p4ui t06, docs/re/UI.md section 9): the skirmish staging list in FrontEnd on a fake provider:
// Official / Custom tabs, row text ("(N) name", missing key, file-name fallback with the 22 + "..." cut), order (space maps
// first, then row text case-insensitively, ties by map id), the initial selection (map started last, else Kuat, else row 0),
// and that Start sends the selected row's map id through the filtered / sorted view. Data-free.
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

struct Rig {
    Env env;
    FrontEnd fe;
    std::vector<SkirmishMapInfo> maps;
    std::vector<MenuAction> acts;
    explicit Rig(std::vector<SkirmishMapInfo> m) : maps(std::move(m)) {
        env.script.merge(kSkirmishRc);
        env.text.add("TEXT_MAP_NAME_KUAT", u"Kuat");
        env.text.add("TEXT_MAP_NAME_ALPHA", u"Alpha");
        env.text.add("TEXT_MAP_NAME_ENDOR", u"Endor");
        MenuProviders p;
        p.skirmish_maps = [this] { return maps; };
        fe.init(env.gui, {}, p);
    }
    void click(const char* dialog, const char* control) {
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, dialog, control)}, InputFrame{}, acts);
    }
    void open_staging() {
        env.gui.close_all();
        env.gui.open(kSingle);
        click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    }
    void select(int32_t row) { env.gui.find(kStaging)->set_selected("IDC_STAGING_MAP_LIST", row); }
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

    // Official tab on open: space maps, then land maps, each by row text case-insensitively; "(N) " + the localised name.
    const std::vector<std::u32string> official = {U"(2) TEXT_NO_SUCH_KEY", U"(4) Kuat", U"(2) Alpha", U"(4) Endor", U"A234567890123456789012345",
                                                  U"ABCDEFGHIJKLMNOPQRSTUV...", U"Twin", U"Twin", U"Zed Label"};
    CHECK(!rig.fe.skirmish_custom_tab());
    CHECK(items_of(rig.env.gui) == official);
    // Initial selection: no map started yet, so Kuat.
    CHECK(selected_of(rig.env.gui) == 1);
    CHECK(rig.fe.skirmish_map_row("_mp_space_kuat") == 1 && rig.fe.skirmish_map_row("_MP_LAND_ENDOR") == 3 && rig.fe.skirmish_map_row("SHORT_NAME") == -1 &&
          rig.fe.skirmish_map_row(kLong) == 5 && rig.fe.skirmish_map_row("nope") == -1);
    // Start sends the selected row's map id, through the sorted view (row 1 is maps_[2]).
    CHECK(rig.start() == "_MP_SPACE_KUAT");
    // Every row maps back to its own map id (row order is not the provider order).
    const char* const ids[9] = {"_MP_SPACE_ABC", "_MP_SPACE_KUAT", "_MP_LAND_ALPHA", "_MP_LAND_ENDOR", "A234567890123456789012345",
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "_MP_LAND_TWIN_A", "_MP_LAND_TWIN_B", "_MP_LAND_LABELED"};
    for (int32_t r = 0; r < 9; ++r) {
        rig.select(r);
        CHECK(rig.start() == ids[r]);
    }

    // Custom tab: a click switches and refills; only the custom maps, the space one first; the file name fallback.
    rig.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(rig.fe.skirmish_custom_tab());
    const std::vector<std::u32string> custom = {U"CSPACE", U"My Very Long Custom Ma...", U"SHORT_NAME", U"Short_Two"};
    CHECK(items_of(rig.env.gui) == custom);
    // Nothing started on this tab is listed yet (the last map started was on the Official tab), no Kuat here: row 0.
    CHECK(selected_of(rig.env.gui) == 0);
    CHECK(rig.start() == "DATA\\CUSTOMMAPS\\CSPACE.TED");
    rig.select(1);
    CHECK(rig.start() == "DATA\\CUSTOMMAPS\\My Very Long Custom Map Name.TED");
    rig.select(3);
    CHECK(rig.start() == "Data/Art/Maps/Short_Two.ted");
    CHECK(rig.fe.skirmish_map_row("DATA\\CUSTOMMAPS\\CSPACE.TED") == 0 && rig.fe.skirmish_map_row("_MP_SPACE_KUAT") == -1);

    // Back to Official: the map started last (Short_Two, a custom map) is not listed here, so Kuat; a started official map is
    // selected again after a round trip through the other tab.
    rig.click(kStaging, "IDC_SELECT_OFFICIAL_MAPS");
    CHECK(!rig.fe.skirmish_custom_tab() && items_of(rig.env.gui) == official);
    CHECK(selected_of(rig.env.gui) == 1);
    rig.select(8);
    CHECK(rig.start() == "_MP_LAND_LABELED");
    rig.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    rig.click(kStaging, "IDC_SELECT_OFFICIAL_MAPS");
    CHECK(selected_of(rig.env.gui) == 8);  // the last map, listed

    // Closing and reopening the dialog: the Official tab again, the last map still selected.
    rig.click(kStaging, "IDC_CANCEL_BUTTON");
    CHECK(rig.fe.top() == kSingle);
    rig.click(kSingle, "IDC_SKIRMISH_GAMES_BUTTON");
    CHECK(rig.fe.top() == kStaging && !rig.fe.skirmish_custom_tab() && items_of(rig.env.gui) == official && selected_of(rig.env.gui) == 8);

    // A fresh list without Kuat starts on row 0; custom maps are not shown on the Official tab and vice versa.
    Rig plain({map_row("_MP_LAND_B", "", 2, false, false), map_row("_MP_LAND_A", "", 2, false, false), map_row("MY_CUSTOM", "", 2, false, true)});
    plain.open_staging();
    CHECK(items_of(plain.env.gui) == (std::vector<std::u32string>{U"_MP_LAND_A", U"_MP_LAND_B"}) && selected_of(plain.env.gui) == 0);
    CHECK(plain.start() == "_MP_LAND_A");
    plain.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(items_of(plain.env.gui) == (std::vector<std::u32string>{U"MY_CUSTOM"}) && plain.start() == "MY_CUSTOM");

    // A tab without maps: empty list, nothing selected, Start sends nothing.
    Rig none({map_row("_MP_LAND_ONLY", "", 2, false, false)});
    none.open_staging();
    none.click(kStaging, "IDC_SELECT_CUSTOM_MAPS");
    CHECK(items_of(none.env.gui).empty() && selected_of(none.env.gui) == -1 && none.start().empty());
    Rig empty(std::vector<SkirmishMapInfo>{});
    empty.open_staging();
    CHECK(items_of(empty.env.gui).empty() && selected_of(empty.env.gui) == -1 && empty.start().empty());
}

}  // namespace test_gui_screens_t
