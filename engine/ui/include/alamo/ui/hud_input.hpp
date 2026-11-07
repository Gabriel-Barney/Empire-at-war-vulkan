// P2 input routing (docs/re/UI.md §4.1-4.2 [H]): per mouse / key event the handlers are offered the event in a
// fixed order and the first consumer wins:
//   button press   overlay -> modal targeting controller -> GUI dialogs -> chat (left only) -> HUD -> world
//                  (middle: overlay -> targeting -> dialogs -> world; the HUD never takes middle presses)
//   button release dialogs -> HUD (only when the HUD owns the press) ...; LMB / RMB releases of a press the WORLD
//                  owns always reach the world, wherever the cursor is (drag-select boxes end over the HUD)
//   move           the world always sees the cursor (camera edge scroll); HUD hover unless overlay / targeting
//   wheel          dialogs first only while a dialog has focus; overlay; targeting; world
//   keys           only to dialogs while a dialog has focus; otherwise to the world (camera keys, hotkeys)
//
// InputRouter turns one raw InputFrame into (a) the frame the world sees - fed to InputState for the camera
// controllers and the OrderTranslator, so HUD clicks never become world orders - and (b) HUD pointer events.
// [port] equivalence: when the HUD (or a dialog) owns a press, the world receives neither that press nor its
// release (the original passes the release to the world, whose handler has no press to finish - a no-op; doing
// the same through InputState would turn the lone release into a click).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "alamo/ui/input.hpp"

namespace alamo::ui {

enum class PointerTarget : uint8_t { None = 0, Overlay, Targeting, Dialog, Chat, Hud, World };

struct RouteHooks {
    std::function<bool()> overlay_active;                  // cinematic / movie overlay (consumes the mouse)
    std::function<bool()> targeting_active;                // modal targeting controller (consumes presses)
    std::function<bool(int32_t x, int32_t y)> dialog_hit;  // a GUI dialog takes a press at the pixel
    std::function<bool()> dialog_focus;                    // a dialog has focus (keys, wheel)
    std::function<bool(int32_t x, int32_t y)> chat_hit;    // the chat box takes a left press
    std::function<int32_t(int32_t x, int32_t y)> hud_hit;  // HUD component under the pixel, -1 = none
    // Whether the HUD component under the cursor takes a press of this button (unset = every left / right press). false
    // routes the press to the world as if no component were there (e.g. GalacticHud::takes_button: right clicks on the
    // g_planet_fleet / g_planet_land_forces map buttons reach the planet picking).
    std::function<bool(int32_t component, Button b)> hud_takes;
};

struct HudPointerEvent {
    enum class Kind : uint8_t { Press, Release, Click };
    Kind kind = Kind::Press;
    Button button = Button::Left;
    int32_t component = -1;  // component under the cursor at the event (press component for Click)
    int32_t x = 0, y = 0;
};

struct RoutedInput {
    InputFrame world;                     // raw frame minus everything not routed to the world
    std::vector<HudPointerEvent> hud;     // this frame's HUD pointer events, in button order
    int32_t hover_component = -1;         // HUD component under the cursor (-1: none or blocked)
    std::array<PointerTarget, kButtonCount> owner{};  // press owner of every held button after this frame
    PointerTarget wheel_target = PointerTarget::None;
    bool keys_to_world = true;
};

class InputRouter {
public:
    void route(const InputFrame& raw, const RouteHooks& hooks, RoutedInput& out);
    PointerTarget owner(Button b) const { return owner_[static_cast<size_t>(b)]; }
    void reset() { owner_ = {}; press_component_ = {}; prev_down_ = 0; }

private:
    std::array<PointerTarget, kButtonCount> owner_{};
    std::array<int32_t, kButtonCount> press_component_{};
    uint8_t prev_down_ = 0;
};

}  // namespace alamo::ui
