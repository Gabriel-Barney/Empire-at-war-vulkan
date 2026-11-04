// alamo_game information feeds (P host): plain-data builders for the P4 screens that show game state - the
// encyclopedia pop-up (type data), the holocron pages (droid log, data chart, tech tree, summaries: galactic sim),
// the story dialog / story summary (StorySession) and the campaign score (galaxy + battle tallies). Presentation only:
// everything reads the sim between ticks (const) and returns ui:: structs; nothing here changes the sim.
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/game/story_script.hpp"
#include "alamo/ui/gui_info_screens.hpp"
#include "alamo/ui/gui_screens.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_panels.hpp"
#include "alamo/ui/hud_world.hpp"

namespace alamo::game {

// Display name of a type: its Text_ID in the MasterTextFile, else the type name.
std::u32string type_label(const GameData& d, const ui::TextDatabase& text, objects::TypeIndex t);

// Encyclopedia entry from the type data (t40, HUD_GAPS §2.2): header = name, icon = the HUD icon, cost = Build_Cost_Credits,
// population = Population_Value, unit_class = Encyclopedia_Unit_Class, abilities = the Unit_Abilities_Data button art,
// strong / weak = the HUD icons of the Encyclopedia_Good_Against / _Vulnerable_To type lists, description = the
// Encyclopedia_Text ids (MasterTextFile) joined. A planet has one row instead, its income [port].
ui::EncyclopediaEntry encyclopedia_entry(const GameData& d, const ui::TextDatabase& text, std::span<const ui::HudTypeInfo> hud_types,
                                         objects::TypeIndex t);

// Holocron pages from the galactic sim. The data chart needs a history: sample() once per frame on the galactic map
// (one sample per galactic day per non-neutral player: credits, combat power, planets owned).
class Holocron {
public:
    void reset() {
        days_.clear();
        players_.clear();
        econ_.clear();
        mil_.clear();
        planets_.clear();
    }
    void sample(const galactic::GalacticSim& g);
    // Conflict results (ConflictManager log) and the local plot set's story dialogs, newest first.
    ui::DroidLogInfo droid_log(const GameSession& s, const GameData& d, const ui::TextDatabase& text) const;
    ui::DataChartInfo chart(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text) const;
    // Tech tree of the player's faction (Underworld: the black-market tree): researched = Tech_Level <= the player's,
    // available = one above, locked = higher; types without a tech level are researched.
    ui::TechTreeInfo tech_tree(const galactic::GalacticSim& g, const GameData& d, std::span<const ui::HudTypeInfo> hud_types,
                               int32_t player, bool space) const;
    // Production / fleets / economy rows of the player's planets (planet row order).
    ui::SummaryInfo summary(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text,
                            std::span<const ui::HudTypeInfo> hud_types, int32_t player) const;
    size_t samples() const { return days_.size(); }

private:
    std::vector<int32_t> days_;
    std::vector<int32_t> players_;                  // sampled players (first sample's order)
    std::vector<std::vector<float>> econ_, mil_, planets_;  // [player slot][sample]
};

// Story dialog scripts (GALACTIC_DIALOGS §2.8): every name a plot set asked to load (lua::StoryModel::dialog_loads: plot
// load, Lua Set_Dialog, save load) goes into the cache once, in request order. `loads` keeps the per-set cursors.
struct StoryScriptLoads {
    std::vector<size_t> cursor;    // per galactic plot set
    const void* tactical = nullptr;  // the tactical plot set the cursor below belongs to
    size_t tactical_cursor = 0;
};
void sync_story_scripts(const GameSession& s, const GameData& d, StoryDialogCache& cache, StoryScriptLoads& loads);

// Fire step 6 records (STORY.md §1.6) of every plot set since `cursor` (advanced), in firing order: the holocron list is
// global (GALACTIC_DIALOGS §2.8), so every player's records count. With the loaded Story_Dialog script chapter
// (story_script.hpp): its title, lines with their TEXTCOLOR colours and WAIT times (TEXT through Story_Var), SFX / DIALOG
// cues, IMAGE, and the event's Add_Dialog_Text lines as they are at EXTRA_TEXT; its MOVIE / MOVIE_ONCE goes to `movies`.
// Without one [port]: the Lua dialog lines, else the dialog name. Title fallback "Chapter n".
struct StoryMovie {
    std::string name;
    bool loop = true;
};
// GALACTIC_DIALOGS §2.1 / STORY.md §1.6 step 3: per returned dialog, Story_Dialog_Popup, Story_Dialog_SFX, the holocron
// record it added, and whether it comes from a random story plot (Random_Plot_*: the IDD_STORY_DIALOG offer, §2.4).
struct StoryDialogMeta {
    bool popup = false;
    bool sfx = false;
    bool random_plot = false;
    std::string dialog;      // Story_Dialog
    std::string tag;         // the record's tag
    int32_t chapter = -1;
    uint64_t holocron_id = 0;
};
bool story_dialog_chapter(const StoryDialogCache& cache, const ui::TextDatabase& text, std::string_view dialog, int32_t chapter,
                          std::string_view var, const std::vector<std::string>& extra, ui::StoryDialogInfo& out, StoryMovie* movie);
std::vector<ui::StoryDialogInfo> new_story_dialogs(const GameSession& s, const StoryDialogCache& cache, const ui::TextDatabase& text, size_t& cursor,
                                                   std::vector<StoryMovie>* movies = nullptr, std::vector<StoryDialogMeta>* meta = nullptr);
// IDD_STORY_SUMMARY_DIALOG from the holocron list (GALACTIC_DIALOGS §2.8, fill_holocron_summary): the tab's rows (labels
// looked up add-if-missing, colours dimmed once read / completed), the selected record's chapter (TITLE, TEXT with
// Story_Var, EXTRA_TEXT lines as stored). row_ids = the records' ids per row; selected_index = the selected record's index
// in the list (the caller marks it read, StorySession::mark_holocron_read), SIZE_MAX = none.
struct HolocronSummaryView {
    ui::StorySummaryInfo info;
    std::vector<uint64_t> row_ids;
    size_t selected_index = SIZE_MAX;
};
HolocronSummaryView holocron_summary(const GameSession& s, const StoryDialogCache& cache, const ui::TextDatabase& text, bool completed,
                                     uint64_t selected_id);

// P4 host: rows of the FoC galactic special panels for an open dialog (GameSession::special_dialog), from the GalacticSim
// getters (galactic special_systems.hpp; docs/re/P4_SIM_SYSTEMS.md). `kind` = galactic::kSpecial*; `hero_id` = the dialog's
// buyer / saboteur / defiler / hacker (0: none, e.g. a preview).
//   black market  bm_offers in item-table order (max 5 shown): name / description / icon from BlackMarketItems.xml, price,
//                 enabled = affordable and not purchased [port], tech icon by the item's faction (0 Rebel, 1 Empire)
//   sabotage      sabotage_targets rows (cost >= 1), enabled = affordable
//   corruption    corruption_choices (slot order): cost, the requirement rows "n/m" of corruption_requirements (missions,
//                 units, planets) and the mission map line, enabled = all four checks (P4 5.2: the dialog's rule)
//   hack          hack_find_super_weapon target, Hack_Super_Weapon_Cost, enabled = a target and the credits
//   slice         slice_rows (GalacticSim::slice_entries, candidate order, every entry) in `slice` (ui::SlicePanel, OI
//                 2026-10-28): type, name / icon of the type, price, chance (the panel prints "Chance n%"), enabled =
//                 affordable; title TEXT_SLICE_DIALOG_TITLE, subtitle TEXT_SLICE_DIALOG_DESCRIPTION
struct SpecialPanelData {
    std::u32string title, subtitle, hero;
    std::vector<ui::BlackMarketItem> market;
    std::vector<ui::SliceRow> slice;
    std::vector<ui::SabotageTarget> sabotage;
    std::vector<ui::CorruptionOption> corruption;
    uint32_t hack_target = 0;
    std::u32string hack_cost;
    std::vector<std::u32string> hack_lines;
    bool hack_enabled = false;
};
SpecialPanelData special_panel(const galactic::GalacticSim& g, const GameData& d, const ui::TextDatabase& text,
                               std::span<const ui::HudTypeInfo> hud_types, int32_t player, uint32_t kind, PlanetIndex planet,
                               uint32_t hero_id);

// Campaign end (Int-verify): the host calls galactic::victory_outcome with the campaign's VictoryRules (victory.hpp)
// instead of the former campaign_outcome stand-in.
struct BattleTally {
    uint32_t kills = 0, losses = 0;  // units destroyed by / of the local player (battle deaths)
};
// Campaign score [port formulas]: military = battles won / fought (conflict log), conquest = planets owned / all
// planets, kill efficiency = kills / (kills + losses); rank title by their mean.
ui::CampaignScoreInfo campaign_score(const GameSession& s, const galactic::GalacticSim& g, const ui::TextDatabase& text, bool victory,
                                     const BattleTally& tally);

}  // namespace alamo::game
