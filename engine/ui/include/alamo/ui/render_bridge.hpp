// Header-only glue from ui::CameraParams to the renderer's camera types (engine/render/camera.hpp).
// Include it only from code that links alamo_render (the app / render); alamo_ui itself does not
// depend on render.
//
//   ui::CameraParams p = tactical_cam.params(aspect);         // or galactic_cam.params(aspect)
//   render::ViewMatrices v = ui::to_view_matrices(p, w, h);   // clip planes + FOV from the params
//   render::fill_frame_cb(v, t, frame_cb);  ...  render::background_view(v) for background scenes.
// p.fade_begin / p.fade_end are Object_Fade_Begin / End for the renderer's distance-fade constants.
#pragma once
#include <cstdint>

#include "alamo/render/camera.hpp"
#include "alamo/ui/camera.hpp"

namespace alamo::ui {

inline render::OrbitCamera to_orbit_camera(const CameraParams& p) {
    render::OrbitCamera c;
    c.target = {p.target.x, p.target.y, p.target.z};
    c.yaw = p.orbit_yaw;
    c.pitch = p.orbit_pitch;
    c.distance = p.distance;
    c.fov_x = p.fov_x;
    return c;
}

// Same view as render::make_view(to_orbit_camera(p), ..., znear, zfar); for galactic params this equals
// render::make_galactic_view (FOV atan(0.5), near 10, far 10000 are already in p).
inline render::ViewMatrices to_view_matrices(const CameraParams& p, uint32_t width, uint32_t height) {
    if (p.up.x == 0.0f && p.up.y == 0.0f && p.up.z == 1.0f && p.orbit_pitch > -1.55f && p.orbit_pitch < 1.55f)
        return render::make_view(to_orbit_camera(p), width, height, p.znear, p.zfar);
    // [port: Gabriel] free tactical camera (roll, pitch past the vertical): an explicit look-at with the params' up vector,
    // the same projection rule (horizontal FOV, D3D RH depth 0..1).
    render::ViewMatrices v;
    v.width = width ? width : 1;
    v.height = height ? height : 1;
    v.eye = {p.eye.x, p.eye.y, p.eye.z};
    v.view = render::look_at_rh(v.eye, {p.target.x, p.target.y, p.target.z}, {p.up.x, p.up.y, p.up.z});
    const float aspect = static_cast<float>(v.width) / static_cast<float>(v.height);
    v.proj = render::perspective_rh(render::fov_y_from_horizontal(p.fov_x, aspect), aspect, p.znear, p.zfar);
    v.view_proj = v.proj * v.view;
    v.view_inv = v.view.inverse_affine();
    return v;
}

}  // namespace alamo::ui
