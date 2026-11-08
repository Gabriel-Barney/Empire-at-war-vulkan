// P4 in-game screens (docs/re/UI.md §3.2 "Screens by dialog"): message boxes, battle end, campaign score, retreat,
// in-game chat, planet info, advanced video options and map selection, as flows over GuiManager dialogs.
// Host API: show_*() with plain data in, GuiManager::update events in (update()), ScreenActions + commands out. The
// screens never read or write sim state; the only sim-affecting click (Retreat) leaves as a commands::EventData.
//
//   IDD_DIALOG_BOX0 / 1 / 2 / BOX  message box with 0 / 1 / 2 / 3 buttons (LEFT / CENTER / RIGHT), IDC_CAPTION + three
//   (+ _CENTERED variants),        user lines IDC_USER_TEXT_STRING_1..3; IDD_DIALOG_BOX_SMALL = caption only
//   IDD_DIALOG_BOX_SMALL           -> MessageBoxResult{tag, button index} (Esc: -1)
//   IDD_BATTLE_END_DIALOG          title, planet, battle time, bounty, losses (IDC_YOUR/ENEMY_UNIT0..11 icons + names,
//                                  IDC_YOUR/ENEMY_HERO0..4), trackbars scroll the loss lists, countdown text;
//                                  Continue -> BattleEndContinue, Invade -> LandInvade, Save replay -> SaveReplay
//   IDD_CAMPAIGN_SCORE_DIALOG      victory text, rank title, military / conquest / kill efficiency, list, image;
//                                  IDC_QUIT_BUTTON -> CampaignScoreClosed
//   IDD_RETREAT_DIALOG             Retreat -> commands::ev::retreat(true) + RetreatConfirmed; Autoresolve -> AutoResolve;
//                                  Cancel / Esc -> RetreatCancelled
//   IDD_INGAME_CHAT_DIALOG         Enter -> SendChat{text, team_only} (the network layer turns it into the non-scheduled
//                                  Chat event 16); Esc -> ChatClosed; IDC_TEAM_MEMBERS_ONLY check box = team_only
//   IDD_PLANET_INFO                GALACTIC_DIALOGS §9 (the organiser's planet_info button):
//                                  "Planet Info <name>" in the owner colour, planet render + load_overlay.tga, wrapped
//                                  tactical (50 200 255) / advantages / history lists, population / terrain / weather texts
//                                  and icons; single instance, no pause; Close / Esc -> PlanetInfoClosed {object = planet}
//   (IDD_AUTO_RESOLVE_DIALOG is not modelled: retail shows it only in multiplayer, which is out of scope)
//   (IDD_ADVANCED_VIDEO_OPTIONS_DIALOG belongs to the options flow: FrontEnd, gui_menus.hpp)
//   IDD_SELECT_MAP_DIALOG          maps list; Select / double click -> MapSelected{map}; Cancel -> MapSelectCancelled
//
// [port] choices: battle-end lists show "name" or "name x N", icons on the PETROGLYPH_DIALOG_IMAGE controls, a
// trackbar value v (0..100) scrolls the 12 rows by round(v / 100 * (n - 12)); IDC_COUNTDOWN_TIMER shows whole seconds
// (hidden when < 0); efficiencies are shown as "N%"; the message box picks BOX0 / BOX1 / BOX2 / BOX by button count
// (BOX1's single button is IDC_DIALOG_BOX_BUTTON_LEFT, BOX2 uses LEFT / RIGHT); lines beyond three are dropped;
// default titles: TEXT_VICTORY / TEXT_DEFEAT when the MasterTextFile has them, else "Victory" / "Defeat".
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_menus.hpp"

namespace alamo::ui {

struct MessageBoxSpec {
    std::string tag;                       // returned with the result
    std::u32string caption;
    std::vector<std::u32string> lines;     // up to 3
    std::vector<std::u32string> buttons;   // 0..3
    bool centered = false;                 // the *_CENTERED variants
    bool small = false;                    // IDD_DIALOG_BOX_SMALL (caption only, no buttons)
};

struct BattleEndUnit {
    std::string icon;       // texture (atlas or image page)
    std::u32string name;
    uint32_t count = 1;
};
struct BattleEndInfo {
    bool victory = true;
    std::u32string title;   // "" = TEXT_VICTORY / TEXT_DEFEAT
    std::u32string planet, time;  // `time`: the whole text; empty -> battle_seconds
    // Playtest 2 t13 (auto-resolve results): TEXT_BATTLE_TIME + hh:mm:ss when `time` is empty and this is >= 0.
    int64_t battle_seconds = -1;
    int64_t bounty = -1;    // < 0: hidden; shown through TEXT_SUMMARY_BOUNTY_COLLECTED ("Salvage collected: $ %d")
    // Credit value of each side's losses (IDC_YOUR/ENEMY_LOSS_VAL_STATIC; appended to the loss header when the dialog
    // template has no such control) and the combat efficiency in percent (IDC_EFFICIENCY_STATIC / STATIC2; appended
    // after TEXT_COMBAT_EFFICIENCY to the loss header otherwise). < 0: not shown.
    int64_t your_loss_value = -1, enemy_loss_value = -1;
    int32_t your_efficiency = -1, enemy_efficiency = -1;
    std::vector<BattleEndUnit> your_losses, enemy_losses;
    std::vector<std::string> your_heroes, enemy_heroes;  // icons, up to 5 each
    bool can_invade = false;
    bool can_save_replay = true;
    float countdown = -1.0f;  // seconds; < 0 hidden
};

struct CampaignScoreInfo {
    bool victory = true;
    std::u32string headline;   // "" = TEXT_VICTORY / TEXT_DEFEAT
    std::u32string rank_title;
    int32_t military = -1, conquest = -1, kill = -1;  // percent; < 0 blank
    std::vector<std::u32string> list;
    std::string image;
};

// GALACTIC_DIALOGS §9.2: everything from the planet TYPE (static XML); no §6.3 gating, no live sim state.
struct PlanetInfoData {
    uint32_t planet = 0;                   // reported by PlanetInfoClosed (object)
    std::u32string name;                   // display name; IDC_STATIC_PLANET_NAME = TEXT_BUTTON_PLANET_INFO + " " + name
    Rgba8 name_color{255, 255, 255, 255};  // the owner faction's colour; white when unowned
    // Paragraphs (one item each; word-wrapped to the list width, scrolled to the top): Describe_Tactical (drawn in
    // kPlanetInfoTacticalColor), Describe_Advantage, Describe_History (skin colour). A text with line breaks can be
    // passed as one item: '\n' splits it.
    std::vector<std::u32string> tactical, advantages, history;
    std::u32string terrain, weather, natives;  // Describe_Terrain / Describe_Weather / Describe_Population
    // IDC_PLANET_ICON: the planet's model rendered by the host (512 x 512, battle-load-screen camera / lights) under a
    // texture name its GuiImageResolver serves. planet_overlay "" = load_overlay.tga.
    std::string planet_icon, planet_overlay;
    // Final texture names: weather_icon = planet_info_weather_icon(Encyclopedia_Weather_Icon), native_icon =
    // Native_Icon_Name, map_icon = planet_info_map_icon(Land_Tactical_Map) or "" when that .dds does not exist (the
    // control is switched off). "" hides a control.
    std::string weather_icon, map_icon, native_icon;
};
inline constexpr Rgba8 kPlanetInfoTacticalColor{50, 200, 255, 255};
inline constexpr float kBattleEndRowsUp = 89.0f;  // [port] loss rows without a hero row: y 259 -> 170, below the header (y 147..162)
// "i_pa_weather_rain.tga" -> "load_weather_rain.tga" (first 4 characters replaced by "load"); "" stays "".
std::string planet_info_weather_icon(std::string_view encyclopedia_weather_icon);
// "Kuat.ted" -> "Kuat.dds" (any-case .ted suffix replaced, else ".dds" appended); "" stays "". Directory: the host's
// (Data\Art\Textures\ [M]).
std::string planet_info_map_icon(std::string_view land_tactical_map);

struct ScreenAction {
    enum class Kind : uint8_t {
        MessageBoxResult,     // tag, button (-1 = Esc)
        BattleEndContinue,
        LandInvade,
        SaveReplay,           // N1 / replay owner
        CampaignScoreClosed,
        RetreatConfirmed,     // the Retreat command was emitted
        AutoResolve,
        RetreatCancelled,
        SendChat,             // text, team_only
        ChatClosed,
        PlanetInfoClosed,
        MapSelected,          // map
        MapSelectCancelled,
        // P4 completion 2 (gui_info_screens.hpp), appended:
        BuyUnit,              // map = unit type (skirmish buy units)
        SellUnit,             // map = unit type
        SellAllUnits,
        BuyUnitsDone,         // Start game
        BuyUnitsBack,
        StoryAccepted,        // tag
        StoryCancelled,       // tag
        StoryClosed,          // tag
        StorySummaryFilter,   // button 0 = active, 1 = completed stories
        StorySummaryClosed,
        InfoPageChanged,      // button = InfoPage
        InfoClosed,
        InfoHintsToggled,     // button = 1 on / 0 off
        SummarySort,          // tag = the column button id (IDC_BUTTON_*)
        SummaryPlanetary,
        StorySummaryGoal,     // GALACTIC_DIALOGS §2.2, appended: IDC_GOAL_LIST row chosen (button = row); host refills the summary
    };
    Kind kind = Kind::MessageBoxResult;
    std::string tag, map;
    int32_t button = -1;
    std::u32string text;
    bool team_only = false;
    uint32_t object = 0;
};
const char* screen_action_name(ScreenAction::Kind k);

// Dialog names.
namespace dlg {
inline constexpr const char* kBattleEnd = "IDD_BATTLE_END_DIALOG";
inline constexpr const char* kCampaignScore = "IDD_CAMPAIGN_SCORE_DIALOG";
inline constexpr const char* kRetreat = "IDD_RETREAT_DIALOG";
inline constexpr const char* kChat = "IDD_INGAME_CHAT_DIALOG";
inline constexpr const char* kPlanetInfo = "IDD_PLANET_INFO";
inline constexpr const char* kSelectMap = "IDD_SELECT_MAP_DIALOG";
}  // namespace dlg

class GameScreens {
public:
    void init(GuiManager& gui) { gui_ = &gui; }

    // Each show_* opens (or refreshes, when already open) its dialog on top; false when the script lacks it.
    bool show_message(const MessageBoxSpec& spec);
    bool show_battle_end(const BattleEndInfo& info);
    void set_countdown(float seconds);  // battle end
    bool show_campaign_score(const CampaignScoreInfo& info);
    bool show_retreat_confirm();
    bool open_chat(bool team_only = false);
    bool chat_open() const;
    // GALACTIC_DIALOGS §9.4: opens IDD_PLANET_INFO (no pause). Single instance: while it is open another call does nothing
    // and returns false (the first planet is kept). The host closes it with close_planet_info() when a battle load screen
    // starts (no action is emitted then).
    bool show_planet_info(const PlanetInfoData& info);
    bool planet_info_open() const;
    void close_planet_info();
    bool show_select_map(std::span<const SkirmishMapInfo> maps);
    void close(std::string_view dialog);
    void close_all();  // every dialog this class opened
    // Message box currently open (its dialog name), "" when none.
    const std::string& message_dialog() const { return msg_dialog_; }

    // Consumes this frame's GUI events (GuiManager::update output); events of other dialogs are ignored.
    void update(std::span<const GuiEvent> events, std::vector<ScreenAction>& out, std::vector<commands::EventData>& commands);

    // Battle-end loss rows currently shown (first row index) per side, for tests.
    int32_t battle_end_offset(bool enemy) const { return enemy ? end_off_[1] : end_off_[0]; }

private:
    GuiDialog* open(const char* name);
    void fill_battle_end_units(GuiDialog& d);
    void layout_battle_end_rows(GuiDialog& d, const BattleEndInfo& info);
    void fill_battle_end_values(GuiDialog& d, const BattleEndInfo& info);
    std::u32string text(std::string_view key, std::u32string_view fallback) const;
    void on_click(const GuiEvent& e, std::vector<ScreenAction>& out, std::vector<commands::EventData>& cmds);
    void on_cancel(const GuiEvent& e, std::vector<ScreenAction>& out);

    GuiManager* gui_ = nullptr;
    std::string msg_dialog_, msg_tag_;
    std::vector<std::string> msg_buttons_;  // control id per button index
    BattleEndInfo end_;
    int32_t end_off_[2] = {0, 0};
    std::u32string end_tail_[2];  // text appended to the battle-end loss headers (removed when the dialog is filled again)
    std::vector<SkirmishMapInfo> maps_;
    uint32_t planet_info_ = 0;
};

}  // namespace alamo::ui
