// alamo_render - camera and matrix conventions (docs/re/RENDER.md 3.2):
//   * world right-handed, Z up; the camera looks along its local -Z;
//   * column vectors, matrices stored as rows (Mat4::m[row][col], p' = M p) - the same "register
//     rows" as materials::FrameCB / DrawData, so rows copy straight into the constant buffers;
//   * projection = D3DXMatrixPerspectiveFovRH with depth 0..1, the camera FOV is HORIZONTAL
//     (fovY = 2 atan(tan(fovX/2) / aspect));
//   * Vulkan: clip-space Y stays "up" (D3D convention) and the viewport has a negative height
//     (y = height, height = -height), which flips Y and keeps D3D's winding.
#pragma once
#include <cstdint>

#include "alamo/assets/math.hpp"
#include "alamo/materials/constants.hpp"

namespace alamo::render {

using assets::Float3;
using assets::Mat3x4;

struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};

struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    static Mat4 identity() { return {}; }
    static Mat4 from(const Mat3x4& a);  // affine, last row (0,0,0,1)
    Mat4 operator*(const Mat4& b) const;
    Float4 operator*(const Float4& v) const;
    Mat4 inverse_affine() const;  // rows 0..2 affine, row 3 = (0,0,0,1)
};

Mat3x4 to_mat3x4(const Mat4& a);  // drops row 3

// View matrix (world -> camera) of a camera at `eye` looking at `target` with world up +Z.
Mat4 look_at_rh(const Float3& eye, const Float3& target, const Float3& up = {0, 0, 1});
// D3DXMatrixPerspectiveFovRH(fov_y, aspect, near, far) in column-vector rows: depth 0 at near, 1 at far.
Mat4 perspective_rh(float fov_y, float aspect, float znear, float zfar);
// Horizontal FOV (the Alamo camera field) -> vertical FOV for an aspect (width / height).
float fov_y_from_horizontal(float fov_x, float aspect);

// Orbit camera around a target: yaw about +Z (0 = camera on +X looking towards -X), pitch above
// the XY plane, distance from the target.
struct OrbitCamera {
    Float3 target{0, 0, 0};
    float yaw = 0.6f;       // radians
    float pitch = 0.35f;    // radians, clamped to (-1.55, 1.55)
    float distance = 100.0f;
    float fov_x = 0.9599311f;  // 55 degrees (TacticalCameras.xml Space_Mode default)
    float radius = 50.0f;      // framed bounding radius (drives near/far)

    Float3 eye() const;
    // Fit a bounding sphere in the view for `aspect` (uses the narrower of the two FOVs).
    void frame(const Float3& center, float sphere_radius, float aspect);
    void orbit(float dyaw, float dpitch);
    void zoom(float wheel_steps);  // > 0 zooms in
    void clip_planes(float& znear, float& zfar) const;
};

struct ViewMatrices {
    Mat4 view, proj, view_proj, view_inv;
    Float3 eye;
    uint32_t width = 1, height = 1;
};
ViewMatrices make_view(const OrbitCamera& cam, uint32_t width, uint32_t height);
// Same with explicit clip planes (the tactical camera: Near_Clip 10 / Far_Clip 7000, RENDER.md 3.4).
ViewMatrices make_view(const OrbitCamera& cam, uint32_t width, uint32_t height, float znear, float zfar);
// TacticalCameras.xml FOV (degrees, defined for 4:3) -> horizontal FOV in radians for a display aspect:
// wider than 4:3 gives 2 atan(tan(fov/2) * 0.75 * 16/9) (RENDER.md 3.4).
float tactical_fov_x(float fov_degrees, float aspect);

// Background-scene camera (SPACE_AND_OPEN.md D.7 2): the SAME camera as `main` - view, eye, view_inv and the
// projection's x/y scale (FOV, aspect) unchanged - with only the depth range replaced by near 10 / far 100000
// (distance fade off is the renderer's part). No re-centring, no rotation-only view.
inline constexpr float kBackgroundNear = 10.0f, kBackgroundFar = 100000.0f;
ViewMatrices background_view(const ViewMatrices& main);

// Galactic camera (SPACE_AND_OPEN.md B.8 [H]): horizontal FOV atan(0.5) = 0.4636 rad (26.57 deg) for every
// display aspect in the original (no Hor+ correction), near 10, far 10000, no distance fade (RecordOptions::galactic for
// the post rules). Galactic background scenes use near 10 / far 100000. [port: Gabriel 2026-11-13] Hor+: atan(0.5) is
// the 4:3 value; wider displays keep 4:3's vertical FOV (galactic_fov_x, same as ui::galactic_fov_x).
inline constexpr float kGalacticFovX = 0.46364761f;  // atan(0.5)
inline constexpr float kGalacticNear = 10.0f, kGalacticFar = 10000.0f;
// Horizontal FOV for a display aspect from the 4:3 one: 2 atan(tan(fov_x_4_3 / 2) x aspect x 3/4) above 4:3, else unchanged.
float galactic_fov_x(float fov_x_4_3, float aspect);
// `cam` with fov_x replaced by galactic_fov_x(kGalacticFovX, width / height) and the galactic clip planes.
ViewMatrices make_galactic_view(const OrbitCamera& cam, uint32_t width, uint32_t height);

// Fills the camera part of FrameCB (matrices, eye, resolution constants) and neutral defaults for
// the rest (fog off, white ambient, light 0 straight down).
void fill_frame_cb(const ViewMatrices& v, float time_seconds, materials::FrameCB& out);
// Register rows of a 3x4 world matrix (DrawData::world / BoneMatrix::rows).
void store_rows(const Mat3x4& m, materials::float4 (&rows)[3]);

}  // namespace alamo::render
