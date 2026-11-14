#include "alamo/ui/hud_input.hpp"

namespace alamo::ui {

void InputRouter::route(const InputFrame& raw, const RouteHooks& h, RoutedInput& out) {
    out.world = raw;
    out.hud.clear();
    const bool overlay = h.overlay_active && h.overlay_active();
    const bool targeting = h.targeting_active && h.targeting_active();
    const bool focus = h.dialog_focus && h.dialog_focus();
    const int32_t x = raw.mouse_x, y = raw.mouse_y;
    const int32_t under = (!overlay && !targeting && h.hud_hit && raw.mouse_inside) ? h.hud_hit(x, y) : -1;
    out.hover_component = under;

    uint8_t world_down = 0, world_pressed = 0, world_released = 0;
    for (size_t i = 0; i < kButtonCount; ++i) {
        const auto b = static_cast<Button>(i);
        const uint8_t bit = button_bit(b);
        const bool was_down = (prev_down_ & bit) != 0;
        const bool now_down = (raw.buttons_down & bit) != 0;
        const bool pressed = (raw.buttons_pressed & bit) != 0 || (now_down && !was_down);
        const bool released = (raw.buttons_released & bit) != 0 || (was_down && !now_down);
        if (pressed) {
            PointerTarget t = PointerTarget::World;
            if (overlay) t = PointerTarget::Overlay;
            else if (targeting) t = PointerTarget::Targeting;
            else if (h.dialog_hit && h.dialog_hit(x, y)) t = PointerTarget::Dialog;
            else if (b == Button::Left && h.chat_hit && h.chat_hit(x, y)) t = PointerTarget::Chat;
            else if (b != Button::Middle && under >= 0 && (!h.hud_takes || h.hud_takes(under, b))) t = PointerTarget::Hud;
            owner_[i] = t;
            press_component_[i] = t == PointerTarget::Hud ? under : -1;
            if (t == PointerTarget::Hud) out.hud.push_back({HudPointerEvent::Kind::Press, b, under, x, y});
        }
        const PointerTarget own = owner_[i];
        if (own == PointerTarget::World) {
            if (now_down) world_down |= bit;
            if (pressed) world_pressed |= bit;
            if (released) world_released |= bit;
        }
        if (released) {
            if (own == PointerTarget::Hud) {
                out.hud.push_back({HudPointerEvent::Kind::Release, b, under, x, y});
                if (under >= 0 && under == press_component_[i])
                    out.hud.push_back({HudPointerEvent::Kind::Click, b, under, x, y});
            } else if (own == PointerTarget::World && b != Button::Middle && under >= 0) {
                // The HUD hears the release too, never as a click (it did not own the press) [port: the original
                // skips this while a drag-select box is active; a lone release is a no-op for HUD components].
                out.hud.push_back({HudPointerEvent::Kind::Release, b, under, x, y});
            }
            if (!now_down) {
                owner_[i] = PointerTarget::None;
                press_component_[i] = -1;
            }
        }
    }
    out.world.buttons_down = world_down;
    out.world.buttons_pressed = world_pressed;
    out.world.buttons_released = world_released;
    out.owner = owner_;
    prev_down_ = raw.buttons_down;

    // Wheel: dialogs while focused, overlay, targeting, else the world (the HUD never takes it).
    out.wheel_target = PointerTarget::None;
    if (raw.wheel != 0.0f) {
        out.wheel_target = focus ? PointerTarget::Dialog
                           : overlay ? PointerTarget::Overlay
                           : targeting ? PointerTarget::Targeting
                                       : PointerTarget::World;
    }
    if (out.wheel_target != PointerTarget::World) out.world.wheel = 0.0f;
    // Keys: only dialogs while one has focus.
    out.keys_to_world = !focus;
    if (focus) {
        out.world.keys_down = {};
        out.world.keys_pressed = {};
    }
}

}  // namespace alamo::ui
