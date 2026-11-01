// Shared by the ui GPU test executables (alamo_ui_draw_tests, alamo_ui_gui_draw_tests): the shared RHI test objects
// plus the renderer and HUD renderer these tests drive.
#pragma once
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/test_fixture.hpp"
#include "alamo/ui/hud_renderer.hpp"

namespace alamo::ui_test {

struct Gpu : testing::Gpu {
    render::Renderer renderer;
    ui::HudRenderer hud;
};

using Offscreen = testing::Offscreen;

}  // namespace alamo::ui_test
