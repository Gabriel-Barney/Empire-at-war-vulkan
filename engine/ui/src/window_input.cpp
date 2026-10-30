#include "alamo/ui/window_input.hpp"

#include "alamo/rhi/window.hpp"

namespace alamo::ui {

InputFrame capture_input(rhi::Window& w, double time_seconds, float dt) {
    static_assert(static_cast<size_t>(rhi::MouseButton::Count) == kButtonCount, "button enums must match");
    InputFrame f;
    f.time = time_seconds;
    f.dt = dt;
    f.width = w.width() ? w.width() : 1;
    f.height = w.height() ? w.height() : 1;
    f.mouse_x = w.mouse_x();
    f.mouse_y = w.mouse_y();
    for (uint32_t k = 0; k < 256; ++k) {
        f.keys_down.set(k, w.key_down(k));
        f.keys_pressed.set(k, w.key_pressed(k));
    }
    for (size_t i = 0; i < kButtonCount; ++i) {
        const auto b = static_cast<rhi::MouseButton>(i);
        const auto bit = static_cast<uint8_t>(1u << i);
        if (w.mouse_down(b)) f.buttons_down |= bit;
        if (w.mouse_pressed(b)) f.buttons_pressed |= bit;
        if (w.mouse_released(b)) f.buttons_released |= bit;
    }
    f.wheel = w.take_wheel();
    const bool inside = f.mouse_x >= 0 && f.mouse_y >= 0 && f.mouse_x < static_cast<int32_t>(f.width) &&
                        f.mouse_y < static_cast<int32_t>(f.height);
    f.mouse_inside = inside || f.buttons_down != 0;  // the window captures the mouse while a button is held
    return f;
}

}  // namespace alamo::ui
