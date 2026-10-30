// rhi::Window -> InputFrame (library alamo_ui_rhi, built when the rhi module is configured).
// Call once per main-loop iteration right after Window::pump() and before any sim tick
// (MAIN_LOOP.md step 3). Consumes the window's edge flags (key_pressed / mouse_pressed / released /
// wheel), so nothing else should read those edges directly.
#pragma once
#include "alamo/ui/input.hpp"

namespace alamo::rhi {
class Window;
}

namespace alamo::ui {

InputFrame capture_input(rhi::Window& window, double time_seconds, float dt);

}  // namespace alamo::ui
