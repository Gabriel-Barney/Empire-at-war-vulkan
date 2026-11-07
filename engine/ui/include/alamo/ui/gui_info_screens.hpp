// P4 completion 2: the remaining game screens (docs/re/UI.md §3.2), same host API as GameScreens (gui_screens.hpp): plain
// data in through show_* / set_*, GuiManager events in through update(), ScreenActions out. Display only - no sim access.
//
//   IDD_SKIRMISH_BUY_UNITS_DIALOG   unit list "name  cost  xN" (IDC_SKIRMISH_BUY_FORCES_LISTBOX), credits balance, selected
//                                   unit image; Buy / Sell (or double click = buy) -> BuyUnit / SellUnit{map = type},
//                                   Sell all, Start game -> BuyUnitsDone, Cancel / Esc -> BuyUnitsBack
//   IDD_STORY_DIALOG                story text lines (IDC_STORY_TEXT), image; Accept / Cancel / Close -> Story*{tag}
//   IDD_STORY_SUMMARY_DIALOG        goals + recap lists, Active / Completed radios -> StorySummaryFilter (host refills),
//                                   Close; Abandon is permanently disabled and has no handler (retail: switched off at
//                                   creation, no callback case; docs/re/P4_SIM_SYSTEMS.md §9.1)
//   (IDD_PLANET_INFO, GALACTIC_DIALOGS §9, is GameScreens::show_planet_info in gui_screens.hpp)
//   IDD_PLANET_DIALOG               [dead in the original, GALACTIC_DIALOGS §9.1: show_planet is kept but unused] name, image, income, political / starbase / ground base level icons (0..4), companies /
//                                   fleets counts, production item + progress; Organization / Production / Cancel
//   "holocron" pages (4 tab radios on every page: Droid log / Data chart / Tech tree / Summary; Close / Esc ->
//   InfoClosed; a tab -> that page + InfoPageChanged):
//     IDD_DROID_LOG                 log list, droid picture, legend rows IDC_PA_ICONn / IDC_PA_TEXTn, hints check
//     IDD_DATA_CHART                Economy / Military / Planets radios, one polyline per player drawn over
//                                   IDC_DATA_CHART_IMAGE (build_overlay), Y max / min / caption, start / middle / end cycle
//     IDD_TECH_TREE_<REBEL|EMPIRE|BLACKMARKET>[_SPACE]   every IDC_TECHTREE_<ObjectType> image from a provider, tinted by
//                                   state; Land / Space radios switch the _SPACE variant
//     IDD_SUMMARY_SCREEN_PRODUCTION / _FLEETS / _ECONOMY   planet rows (6 / 10 / 6 visible, IDC_SCROLLBAR_SLIDER scrolls),
//                                   sub-page radios; column buttons -> SummarySort{tag}, Planetary -> SummaryPlanetary
// Tech tree art (UI.md §3.7.2): IDC_RIGHT_ARROWn / _DRIGHT_ / _RRIGHT_ / _DOWN_ get i_tree_arrow_right / _2right / _3right /
// _down.tga, white; the fleet summary's IDC_HEROES_DIVIDERS(r-1) is the right arrow above each filled row r >= 1.
// [port] choices: tech icon tint locked 90 grey, available and researched white (the original has no researched colour;
// TechState keeps the value); the data chart is drawn as 2-unit
// dots every 2 units along each segment, the y range 0..max(values) (min 0); summary trackbar v scrolls by
// round(v / 100 * (rows - visible)); missing rows / icons are hidden.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_screens.hpp"

namespace alamo::ui {

struct BuyUnitEntry {
    std::string type;
    std::u32string name;
    int32_t cost = 0;
    uint32_t owned = 0;
    std::string icon;
    bool can_buy = true;
};
struct BuyUnitsState {
    int64_t credits = 0;
    std::vector<BuyUnitEntry> units;
};

// A cue of a story dialog script played when the dialog's clock reaches `at` (seconds after the first update_story):
// SFX = a GUI SFX event, DIALOG = a speech event.
struct StoryDialogCue {
    double at = 0.0;
    enum class Kind : uint8_t { Sfx, Speech } kind = Kind::Sfx;
    std::string event;
};
struct StoryDialogInfo {
    std::string tag;
    std::u32string title;
    std::vector<std::u32string> lines;
    // Optional, parallel to `lines`: the line's colour (alpha 0 = the skin's font colours) and the time it appears (seconds
    // after the first update_story; missing = at once). Script WAITs make the text appear line by line.
    std::vector<Rgba8> line_colors;
    std::vector<double> line_at;
    std::vector<StoryDialogCue> cues;  // ascending `at`
    std::string image;
    bool accept = true, cancel = false, close = true;  // buttons shown
};
// GALACTIC_DIALOGS §2.2 / §2.8: goals = the selected tab's holocron records, newest first (IDC_GOAL_LIST, `selected`
// preselected): each row's label is its chapter's first TEXT line, drawn in goal_colors (parallel; the TEXTCOLOR before
// that TEXT, default 128,128,255, x 0.66 once read or completed; missing = skin colours); title = the selected chapter's
// TITLE text (the IDC_STATIC above the recap, placeholder TEXT_STORY_GOALS); recap = its body lines (recap_colors
// parallel, alpha 0 = skin colours: TEXTCOLOR white 255,255,255,176 / red 255,24,39,255), wrapped in the list (text
// escapes for line breaks become breaks there, §2.7). Completed is disabled while has_completed is false; Abandon hidden;
// IDC_HINTS = TEXT_STORY_HINT (data). A goal click emits ScreenAction::StorySummaryGoal (button = row); the host refills
// with that record. The host holds the galactic pause while it is open.
struct StorySummaryInfo {
    std::vector<std::u32string> goals, recap;
    std::vector<Rgba8> goal_colors;
    bool completed = false;     // Completed radio checked
    std::u32string title;       // chapter TITLE line ("" = keep the placeholder)
    std::vector<Rgba8> recap_colors;
    int32_t selected = -1;      // goal row preselected (-1 = the last row)
    bool has_completed = true;  // false: the Completed tab is disabled
};

struct PlanetDialogInfo {
    uint32_t planet = 0;
    std::u32string name;
    std::string image;
    int64_t credits = -1;
    std::vector<std::string> political, starbase, ground_base;  // up to 5 icons each
    // REMAINING_GAPS §6.3 display gates (host: GalacticSim::can_see_starbase_info / can_see_groundbase_info for the local
    // player): false blanks the IDC_STARBASE_ICON_n / IDC_GROUND_BASE_ICON_n row whatever `starbase` / `ground_base` hold.
    bool starbase_info = true, groundbase_info = true;
    std::string companies_icon, fleets_icon;
    int32_t companies = -1, fleets = -1;
    std::u32string producing;   // "" = nothing in production
    float progress = -1.0f;     // 0..1
};

enum class InfoPage : uint8_t { DroidLog, DataChart, TechTree, SummaryProduction, SummaryFleets, SummaryEconomy };

struct DroidLogInfo {
    std::vector<std::u32string> entries;
    std::string picture;
    std::vector<std::pair<std::string, std::u32string>> legend;  // (icon, text) for IDC_PA_ICONn / IDC_PA_TEXTn, n = 0..24
    bool hints = true;
};
struct ChartSeries {
    std::u32string name;
    Rgba8 color{255, 255, 255, 255};
    std::vector<float> values;  // one per cycle
};
struct DataChartInfo {
    std::vector<ChartSeries> economy, military, planets;
    int32_t first_cycle = 0;
    std::u32string economy_caption = U"Credits", military_caption = U"Military", planets_caption = U"Planets";
};
enum class TechState : uint8_t { Locked, Available, Researched };
struct TechTreeItem {
    std::string icon;
    TechState state = TechState::Locked;
};
struct TechTreeInfo {
    // modc t20: the player faction's Tech_Tree_Dialog_Name (FoC IDD_TECH_TREE_REBEL / _EMPIRE / _BLACKMARKET); "" = the
    // faction has no tech tree (the page does not open).
    std::string dialog;
    bool space = false;
    // Object type name (the IDC_TECHTREE_<name> suffix) -> item; false hides the control.
    std::function<bool(std::string_view type, TechTreeItem& out)> item;
};
struct SummaryProductionRow {
    std::string icon;
    std::u32string name;
    std::string space_icon, ground_icon;
    std::u32string space_text, ground_text;
    std::vector<std::string> heroes;  // up to 8
    std::u32string pop;
};
struct SummaryFleetRow {
    std::string icon;
    std::u32string name;
    std::vector<std::string> fleets;  // up to 15
};
struct SummaryEconomyRow {
    std::string icon;
    std::u32string name;
    std::u32string total, base, trade, mining;
    std::string mining_icon;
    int32_t mining_count = 0;
};
struct SummaryInfo {
    std::vector<SummaryProductionRow> production;
    std::u32string pop_total;
    std::vector<SummaryFleetRow> fleets;
    std::vector<SummaryEconomyRow> economy;
    std::u32string total_income, total_base, total_trade, total_mining;
};

namespace dlg {
inline constexpr const char* kBuyUnits = "IDD_SKIRMISH_BUY_UNITS_DIALOG";
inline constexpr const char* kStory = "IDD_STORY_DIALOG";
inline constexpr const char* kStorySummary = "IDD_STORY_SUMMARY_DIALOG";
inline constexpr const char* kPlanetDialog = "IDD_PLANET_DIALOG";
inline constexpr const char* kDroidLog = "IDD_DROID_LOG";
inline constexpr const char* kDataChart = "IDD_DATA_CHART";
inline constexpr const char* kSummaryProduction = "IDD_SUMMARY_SCREEN_PRODUCTION";
inline constexpr const char* kSummaryFleets = "IDD_SUMMARY_SCREEN_FLEETS";
inline constexpr const char* kSummaryEconomy = "IDD_SUMMARY_SCREEN_ECONOMY";
}  // namespace dlg
// `dialog` (TechTreeInfo::dialog, the faction's Tech_Tree_Dialog_Name) + "_SPACE" in space; "" when `dialog` is empty.
std::string tech_tree_dialog(std::string_view dialog, bool space);

class InfoScreens {
public:
    void init(GuiManager& gui) { gui_ = &gui; }

    bool show_buy_units(const BuyUnitsState& s);
    void set_buy_units(const BuyUnitsState& s);  // refresh (credits / owned counts)
    bool show_story(const StoryDialogInfo& s);
    // Story dialog clock (call every frame with the wall clock): the first call after show_story starts it; lines whose
    // time came are shown (the list scrolls to the newest), cues whose time came are appended to `due` (once each).
    // True while lines or cues are still waiting.
    bool update_story(double now, std::vector<StoryDialogCue>* due = nullptr);
    size_t story_lines_shown() const { return story_shown_; }
    bool story_waiting() const { return story_shown_ < story_.lines.size() || story_cue_ < story_.cues.size(); }
    bool show_story_summary(const StorySummaryInfo& s);
    bool show_planet(const PlanetDialogInfo& p);  // IDD_PLANET_DIALOG: never used by the original (GALACTIC_DIALOGS §9.1)
    // The holocron: data for every page, then open one (switching pages keeps the data).
    void set_droid_log(DroidLogInfo d) { droid_ = std::move(d); }
    void set_data_chart(DataChartInfo c) { chart_ = std::move(c); }
    void set_tech_tree(TechTreeInfo t) { tech_ = std::move(t); }
    void set_summary(SummaryInfo s) { summary_ = std::move(s); }
    bool show_info(InfoPage page);
    InfoPage info_page() const { return page_; }
    std::string info_dialog() const;  // the open holocron page's dialog, "" when closed
    int32_t chart_mode() const { return chart_mode_; }  // 0 economy, 1 military, 2 planets
    int32_t summary_offset() const { return summary_off_; }
    void close_all();

    // Data-chart lines over IDC_DATA_CHART_IMAGE (call after GuiManager::build, into the same list). Returns the dots.
    size_t build_overlay(HudDrawList& dl) const;

    void update(std::span<const GuiEvent> events, std::vector<ScreenAction>& out);

private:
    GuiDialog* go_info(const std::string& name);
    bool info_dialog(std::string_view n) const;
    void fill_buy(GuiDialog& d);
    void fill_droid(GuiDialog& d);
    void fill_chart(GuiDialog& d);
    void fill_tech(GuiDialog& d);
    void fill_summary(GuiDialog& d);
    void fill_tabs(GuiDialog& d);
    std::u32string text(std::string_view key, std::u32string_view fallback) const;
    void on_click(const GuiEvent& e, std::vector<ScreenAction>& out);
    void on_change(const GuiEvent& e, std::vector<ScreenAction>& out);

    GuiManager* gui_ = nullptr;
    BuyUnitsState buy_;
    std::string story_tag_;
    StoryDialogInfo story_;          // the open story dialog's script (lines revealed by update_story)
    double story_t0_ = -1.0;         // clock start (-1 = not started)
    size_t story_shown_ = 0, story_cue_ = 0;
    void fill_story_lines(GuiDialog& d);
    uint32_t planet_ = 0;
    DroidLogInfo droid_;
    DataChartInfo chart_;
    TechTreeInfo tech_;
    SummaryInfo summary_;
    InfoPage page_ = InfoPage::DroidLog;
    std::string info_open_;
    int32_t chart_mode_ = 0;
    int32_t summary_off_ = 0;
};

}  // namespace alamo::ui
