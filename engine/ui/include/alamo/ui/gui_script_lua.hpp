// alamo_ui_lua (built only when engine/lua is configured): engine/lua -> P4 UI adapters.
//   * gui_script_calls: the GUI_* presentation events of a PresentationLog (PresentationOp::GuiButtonIcon ..
//     GuiTextColor) as GuiScriptCalls, in log order (consume them, then PresentationLog::clear_events);
//   * hud_objectives: the StoryModel's objectives (StorySession) for the galactic / tactical objectives panel.
#pragma once
#include <vector>

#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/gui_script.hpp"

namespace alamo::lua {
class PresentationLog;
class StoryModel;
}  // namespace alamo::lua

namespace alamo::ui {

// Appends the GUI_* calls of the log's current events to `out`; returns how many.
size_t gui_script_calls(const lua::PresentationLog& log, std::vector<GuiScriptCall>& out);
std::vector<HudObjective> hud_objectives(const lua::StoryModel& story);

}  // namespace alamo::ui
