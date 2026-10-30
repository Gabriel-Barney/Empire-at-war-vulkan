// P4 load screens (docs/re/UI.md §5): the battle / mission load dialogs while the LoadThread runs.
//   1. open(): picks IDD_LAND_BATTLE_LOAD_DIALOG / IDD_SPACE_BATTLE_LOAD_DIALOG / IDD_SKIRMISH_BATTLE_LOAD_DIALOG /
//      IDD_LAND_MISSION_LOAD_DIALOG / IDD_SPACE_MISSION_LOAD_DIALOG, fills planet name, terrain / weather / natives texts
//      and icons, the historical-reference list and the player names; IDC_BEGIN_BUTTON is HIDDEN; the host stops the
//      sim (FS +0x34, MAIN_LOOP §2.4) and starts its load worker;
//   2. every frame update() polls the progress callback (single player: IDC_PROGRESS_SOLO; multiplayer: IDC_PROGRESS0..8
//      per player next to IDC_PLAYER_NAME_0..8) - IDC_STATIC_LOADING shows TEXT_SAVELOAD_LOADING_MSG;
//   3. done: progress full; single player (and game type 3): Begin is SHOWN and FOCUSED and GUI_Text_Hint_SFX plays (the
//      GuiManager's SFX hook); the player clicks TEXT_BUTTON_BEGIN (or Enter / Space) -> LoadScreenEvent::Begin. In
//      multiplayer the dialog closes when every player reports done -> Begin at once.
// Music: the host plays GameConstants Music_Event_Battle_Load_Screen while it is open (kLoadScreenMusicConstant).
// [port]: the unused progress bars / name rows of a multiplayer screen are hidden; single player hides IDC_PROGRESS0..8
// and the name rows; planet / weather / native icons go through GuiDialog::set_image (atlas or image pages); the dialog
// opens over a black backdrop (the scene loading behind it stays hidden) and a terrain / weather / population caption with
// no text is hidden with its label; the lists are narrowed to the art's left panel (kPlanetListWidth).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/ui/gui_dialog.hpp"

namespace alamo::ui {

inline constexpr const char* kLoadScreenMusicConstant = "Music_Event_Battle_Load_Screen";
inline constexpr const char* kLoadScreenReadySfx = "GUI_Text_Hint_SFX";

enum class LoadScreenKind : uint8_t { LandBattle, SpaceBattle, Skirmish, LandMission, SpaceMission };
const char* load_screen_dialog(LoadScreenKind k);

struct LoadScreenInfo {
    std::u32string planet_name;
    std::u32string terrain, weather, natives;       // texts (already localised)
    std::string planet_icon, planet_overlay, weather_icon, map_icon, native_icon;  // textures ("" = hidden; overlay "" = load_overlay.tga)
    std::vector<std::u32string> tactical, advantages, history;  // the planet text lists (paragraphs, wrapped to the list)
    std::vector<std::u32string> players;            // multiplayer: one row per player (max 9)
    bool multiplayer = false;
    bool begin_button = true;  // single player / game type 3: wait for Begin; false: close as soon as loaded
};

struct LoadScreenEvent {
    enum class Kind : uint8_t { Ready, Begin } kind = Kind::Ready;
};

class LoadScreen {
public:
    // Opens the dialog (modal) and fills it. False when the script has no such dialog.
    bool open(GuiManager& gui, LoadScreenKind kind, const LoadScreenInfo& info);
    void close();
    bool is_open() const { return gui_ != nullptr; }
    // Progress 0..1 of the local load (single player) - polled every update.
    void set_progress_source(std::function<float()> f) { progress_ = std::move(f); }
    // Multiplayer: load-progress packets per player.
    void set_player_progress(size_t player, float fraction);
    // Polls progress; consumes this frame's GUI events (Begin click). Ready is posted once when loading completes,
    // Begin when the player starts (single player) or every peer is done (multiplayer); the dialog closes on Begin.
    void update(std::span<const GuiEvent> events, std::vector<LoadScreenEvent>& out);
    bool loading() const { return is_open() && !ready_; }
    bool ready() const { return ready_; }
    float progress() const { return local_; }
    const std::string& dialog() const { return dialog_; }

private:
    GuiManager* gui_ = nullptr;
    std::string dialog_;
    std::function<float()> progress_;
    std::vector<float> players_;
    LoadScreenInfo info_;
    float local_ = 0.0f;
    bool ready_ = false;
};

}  // namespace alamo::ui
