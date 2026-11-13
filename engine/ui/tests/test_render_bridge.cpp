// alamo_ui_render_bridge_tests: the renderer's view (render::make_view via ui::to_view_matrices) and the
// UI's picking view (ui::make_view) must agree, so a screen pick hits what is drawn under the cursor.
#include <cmath>
#include <cstdio>

#include "alamo/render/camera.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/render_bridge.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;

using ::alamo::test::g_fail;

namespace {

bool render_project(const render::ViewMatrices& v, ui::Vec3 p, float& sx, float& sy) {
    const render::Float4 c = v.view_proj * render::Float4{p.x, p.y, p.z, 1.0f};
    if (!(c.w > 0.0f)) return false;
    sx = (c.x / c.w + 1.0f) * 0.5f * static_cast<float>(v.width);
    sy = (1.0f - c.y / c.w) * 0.5f * static_cast<float>(v.height);
    return true;
}

void compare(const ui::CameraParams& p, uint32_t w, uint32_t h) {
    const render::ViewMatrices rv = ui::to_view_matrices(p, w, h);
    const ui::View uv = ui::make_view(p, w, h);
    CHECK(std::fabs(rv.eye.x - p.eye.x) < 0.01f && std::fabs(rv.eye.y - p.eye.y) < 0.01f &&
          std::fabs(rv.eye.z - p.eye.z) < 0.01f);
    const ui::Vec3 pts[] = {p.target, {100, 50, 0}, {-250, 300, 40}, {p.target.x + 30, p.target.y - 20, 5}};
    for (const ui::Vec3& q : pts) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        const bool a = render_project(rv, q, ax, ay), b = ui::project(uv, q, bx, by);
        CHECK(a == b);
        if (a && b) CHECK(std::fabs(ax - bx) < 0.05f && std::fabs(ay - by) < 0.05f);
    }
}

}  // namespace

int main() {
    ui::TacticalCameraController space;
    space.set_look_at({120, -40, 0});
    space.set_yaw_deg(35.0f);
    space.rotate(0.0f, -12.0f);
    compare(space.params(16.0f / 9.0f), 1920, 1080);
    compare(space.params(4.0f / 3.0f), 1024, 768);
    ui::TacticalCameraController land(ui::land_mode_defaults());
    land.set_yaw_deg(-70.0f);
    compare(land.params(16.0f / 9.0f), 1280, 720);
    // [port: Gabriel] free camera: rolled, pitched past the vertical (explicit look-at path) and with a FOV setting.
    ui::TacticalCameraController free_cam;
    free_cam.set_look_at({60, 30, 0});
    free_cam.rotate(25.0f, 70.0f);  // pitch 120: over the top
    free_cam.roll(-35.0f);
    free_cam.set_fov_override(90.0f);
    const ui::CameraParams fp = free_cam.params(16.0f / 9.0f);
    CHECK(!(fp.up.x == 0.0f && fp.up.y == 0.0f && fp.up.z == 1.0f));
    compare(fp, 1920, 1080);
    free_cam.roll(35.0f);  // no roll, pitch 120: still the explicit path
    compare(free_cam.params(4.0f / 3.0f), 1024, 768);
    // render::tactical_fov_x and ui::tactical_fov_x agree
    for (float a : {1.25f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 2.4f})
        CHECK(std::fabs(render::tactical_fov_x(55.0f, a) - ui::tactical_fov_x(55.0f, a)) < 1e-6f);
    // galactic Hor+ [port: Gabriel]: render::galactic_fov_x and ui::galactic_fov_x agree; 4:3 and narrower keep the FOV
    for (float a : {1.25f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 2.4f, 32.0f / 9.0f})
        CHECK(std::fabs(render::galactic_fov_x(ui::kGalacticFovX, a) - ui::galactic_fov_x(ui::kGalacticFovX, a)) < 1e-6f);
    CHECK(ui::galactic_fov_x(ui::kGalacticFovX, 1.25f) == ui::kGalacticFovX && ui::galactic_fov_x(ui::kGalacticFovX, 2.4f) > ui::galactic_fov_x(ui::kGalacticFovX, 16.0f / 9.0f));
    // galactic: equal to render::make_galactic_view
    ui::GalacticCameraController g;
    const ui::CameraParams gp = g.params(16.0f / 9.0f);
    compare(gp, 1920, 1080);
    const render::ViewMatrices a = ui::to_view_matrices(gp, 1920, 1080);
    const render::ViewMatrices b = render::make_galactic_view(ui::to_orbit_camera(gp), 1920, 1080);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) CHECK(a.view_proj.m[i][j] == b.view_proj.m[i][j]);
    if (g_fail) {
        std::printf("alamo_ui_render_bridge_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_render_bridge_tests: all passed\n");
    return 0;
}
