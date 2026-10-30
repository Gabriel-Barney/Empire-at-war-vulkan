// OI host (alamo_game): the camera / controls decisions of docs/PHASE4.md "Camera and controls" (Gabriel, 2026-10-27)
// on the host side. The UI owns the tables (ui::HotkeyTable: the 160-slot hotkey table; ui::Bindings: the P1 camera /
// order actions); the host keeps them in step:
//   sync_control_bindings   at start-up and after the options are applied: the hotkey table's camera movement
//                           (W A S D, any modifier), free-camera yaw / pitch / roll, pause, select all / stop and
//                           control-group keys -> Bindings (HotkeyTable::apply_to_bindings), plus the queue modifier
//                           of the camera settings (shift+click queued orders by default). Without it the host ran on
//                           ui::Bindings' own defaults (arrows scroll as well, yaw on Delete / PageDown) until the
//                           player pressed Accept in the controls dialog.
//   check_control_decisions the decisions as rules over the two tables as shipped by default (one message per
//                           violation, empty = all hold; a player's own rebinding may legitimately break them):
//                           W A S D bound to nothing but camera movement (no slot, with or without Ctrl / Shift / Alt);
//                           movement on W A S D matching any modifier; a bound pause action and hotkey; bound yaw /
//                           pitch / roll keys for the free 360-degree tactical camera; bound keyboard zoom in / out
//                           off End / Home and a bound camera reset (engine/ui defaults: + / -, End reset, Home
//                           camera home); a non-zero queue modifier.
// Presentation only (never read by the sim). Needs engine/ui's alamo_ui_hud (ALAMO_GAME_HAVE_CONTROLS).
#pragma once
#ifdef ALAMO_GAME_HAVE_CONTROLS
#include <string>
#include <vector>

#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/input.hpp"

namespace alamo::game {

void sync_control_bindings(const ui::HotkeyTable& hotkeys, const ui::CameraSettings& camera, ui::Bindings& bindings);
std::vector<std::string> check_control_decisions(const ui::HotkeyTable& hotkeys, const ui::Bindings& bindings);

}  // namespace alamo::game
#endif
