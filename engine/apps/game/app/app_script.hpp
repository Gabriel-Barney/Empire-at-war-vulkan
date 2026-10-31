// alamo_game App: the scripted smoke steps shared by app_script.cpp (skirmish / land / campaign smokes) and
// app_script_saveload.cpp (the save / load menus smoke, --smoke saveload).
#pragma once
#include "game_app.hpp"

namespace alamo_game_app::script {

// A click as three frames: move, press, release (InputState: a click without a drag); true once done.
struct ClickState {
    int phase = 0;
};
bool click_at(ClickState& c, ui::InputFrame& f, int32_t x, int32_t y, ui::Button b, bool shift = false);
// Done once `cond` holds.
ScriptStep step_wait(std::string name, std::function<bool(App&)> cond, uint32_t timeout = 3600);
// A left click on a control of `dialog` once that dialog is the topmost one.
ScriptStep step_click_control(std::string dialog, std::string control);
// Selects list row `row` like a player: click the first row (selects + focuses the list), then Down until it is reached.
ScriptStep step_select_row(std::string dialog, std::string control, std::function<size_t(App&)> row);
// A PNG of the next settled frame into --png-dir (the sim held meanwhile: presentation only).
ScriptStep step_shot(std::string file);
// Done once the session ran `n` more ticks.
ScriptStep step_ticks(std::string name, uint64_t n);
// One key press.
ScriptStep step_key(std::string name, uint16_t key);
// Galactic: a human combat fleet ordered (selected, then right-click) at the nearest defended hostile planet.
ScriptStep step_galactic_attack();

// --smoke saveload (app_script_saveload.cpp): the steps after the main menu is shown.
void add_saveload_steps(std::vector<ScriptStep>& s);

}  // namespace alamo_game_app::script
