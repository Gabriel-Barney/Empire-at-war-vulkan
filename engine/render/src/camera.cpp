#include "alamo/render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::render {

namespace {
Float3 sub(const Float3& a, const Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 cross(const Float3& a, const Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 normalize(const Float3& v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0 ? Float3{v.x / l, v.y / l, v.z / l} : Float3{0, 0, 1};
}
}  // namespace

Mat4 Mat4::from(const Mat3x4& a) {
    Mat4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][j];
    return r;
}

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

Float4 Mat4::operator*(const Float4& v) const {
    const float in[4] = {v.x, v.y, v.z, v.w};
    float o[4];
    for (int i = 0; i < 4; ++i) o[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3] * in[3];
    return {o[0], o[1], o[2], o[3]};
}

Mat3x4 to_mat3x4(const Mat4& a) {
    Mat3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][j];
    return r;
}

Mat4 Mat4::inverse_affine() const { return from(to_mat3x4(*this).inverse()); }

Mat4 look_at_rh(const Float3& eye, const Float3& target, const Float3& up) {
    const Float3 f = normalize(sub(target, eye));
    Float3 s = cross(f, up);
    if (dot(s, s) < 1e-12f) s = cross(f, Float3{0, 1, 0});  // looking straight along up
    s = normalize(s);
    const Float3 u = cross(s, f);
    Mat4 v;
    v.m[0][0] = s.x; v.m[0][1] = s.y; v.m[0][2] = s.z; v.m[0][3] = -dot(s, eye);
    v.m[1][0] = u.x; v.m[1][1] = u.y; v.m[1][2] = u.z; v.m[1][3] = -dot(u, eye);
    v.m[2][0] = -f.x; v.m[2][1] = -f.y; v.m[2][2] = -f.z; v.m[2][3] = dot(f, eye);
    return v;
}

Mat4 perspective_rh(float fov_y, float aspect, float zn, float zf) {
    const float ys = 1.0f / std::tan(fov_y * 0.5f);
    const float xs = ys / aspect;
    Mat4 p;
    p.m[0][0] = xs;
    p.m[1][1] = ys;
    p.m[2][2] = zf / (zn - zf);
    p.m[2][3] = zn * zf / (zn - zf);
    p.m[3][2] = -1.0f;
    p.m[3][3] = 0.0f;
    return p;
}

float fov_y_from_horizontal(float fov_x, float aspect) { return 2.0f * std::atan(std::tan(fov_x * 0.5f) / aspect); }

Float3 OrbitCamera::eye() const {
    const float cp = std::cos(pitch);
    return {target.x + distance * cp * std::cos(yaw), target.y + distance * cp * std::sin(yaw),
            target.z + distance * std::sin(pitch)};
}

void OrbitCamera::frame(const Float3& center, float r, float aspect) {
    target = center;
    radius = r > 1e-4f ? r : 1.0f;
    const float fy = fov_y_from_horizontal(fov_x, aspect);
    const float half = 0.5f * std::min(fov_x, fy);
    distance = radius / std::sin(half) * 1.05f;
}

void OrbitCamera::orbit(float dyaw, float dpitch) {
    yaw += dyaw;
    pitch = std::clamp(pitch + dpitch, -1.55f, 1.55f);
}

void OrbitCamera::zoom(float steps) {
    distance *= std::pow(0.85f, steps);
    distance = std::clamp(distance, radius * 0.05f, radius * 50.0f);
}

void OrbitCamera::clip_planes(float& zn, float& zf) const {
    zf = distance + radius * 1.5f;
    zn = std::max(distance - radius * 1.5f, zf * 1e-4f);
}

ViewMatrices make_view(const OrbitCamera& cam, uint32_t width, uint32_t height) {
    ViewMatrices v;
    v.width = width ? width : 1;
    v.height = height ? height : 1;
    const float aspect = static_cast<float>(v.width) / static_cast<float>(v.height);
    float zn = 1, zf = 1000;
    cam.clip_planes(zn, zf);
    v.eye = cam.eye();
    v.view = look_at_rh(v.eye, cam.target);
    v.proj = perspective_rh(fov_y_from_horizontal(cam.fov_x, aspect), aspect, zn, zf);
    v.view_proj = v.proj * v.view;
    v.view_inv = v.view.inverse_affine();
    return v;
}

ViewMatrices make_view(const OrbitCamera& cam, uint32_t width, uint32_t height, float zn, float zf) {
    ViewMatrices v;
    v.width = width ? width : 1;
    v.height = height ? height : 1;
    const float aspect = static_cast<float>(v.width) / static_cast<float>(v.height);
    v.eye = cam.eye();
    v.view = look_at_rh(v.eye, cam.target);
    v.proj = perspective_rh(fov_y_from_horizontal(cam.fov_x, aspect), aspect, zn, zf);
    v.view_proj = v.proj * v.view;
    v.view_inv = v.view.inverse_affine();
    return v;
}

float tactical_fov_x(float fov_degrees, float aspect) {
    const float f = fov_degrees * 0.017453293f;
    if (aspect <= 4.0f / 3.0f + 1e-4f) return f;
    return 2.0f * std::atan(std::tan(0.5f * f) * 0.75f * 16.0f / 9.0f);
}

ViewMatrices background_view(const ViewMatrices& main) {
    ViewMatrices b = main;
    // A plain perspective_rh with the main camera's x / y scale (FOV, aspect) kept bit for bit; anything else in
    // the main projection (an oblique water-reflection clip plane in row 2) is not inherited.
    Mat4 p;
    p.m[0][0] = main.proj.m[0][0];
    p.m[1][1] = main.proj.m[1][1];
    // [port] TAA sub-pixel jitter (taa.hpp jitter_view: rows 0 / 1 + o x row 3, row 3 shared): the background moves
    // with the scene. Zero without TAA (and in every plain perspective_rh), so nothing changes then.
    p.m[0][2] = main.proj.m[0][2];
    p.m[1][2] = main.proj.m[1][2];
    p.m[2][2] = kBackgroundFar / (kBackgroundNear - kBackgroundFar);
    p.m[2][3] = kBackgroundNear * kBackgroundFar / (kBackgroundNear - kBackgroundFar);
    p.m[3][2] = -1.0f;
    p.m[3][3] = 0.0f;
    b.proj = p;
    b.view_proj = b.proj * b.view;
    return b;
}

ViewMatrices make_galactic_view(const OrbitCamera& cam, uint32_t width, uint32_t height) {
    OrbitCamera c = cam;
    c.fov_x = kGalacticFovX;  // fixed: only the aspect follows the display
    return make_view(c, width, height, kGalacticNear, kGalacticFar);
}

namespace {
void rows4(const Mat4& m, materials::float4 (&out)[4]) {
    for (int i = 0; i < 4; ++i) out[i] = {m.m[i][0], m.m[i][1], m.m[i][2], m.m[i][3]};
}
}  // namespace

void fill_frame_cb(const ViewMatrices& v, float t, materials::FrameCB& o) {
    o = {};
    rows4(v.view, o.view);
    rows4(v.proj, o.proj);
    rows4(v.view_proj, o.view_proj);
    rows4(v.view_inv, o.view_inv);
    o.eye_pos = {v.eye.x, v.eye.y, v.eye.z, 1.0f};
    const float w = static_cast<float>(v.width), h = static_cast<float>(v.height);
    o.resolution_constants = {w, h, 0.5f / w, 0.5f / h};
    // FOG_VALS default start 10000 / end 15000 and DISTANCE_FADE_VALS "off" (RENDER.md 2.2).
    o.fog_vals_fade_vals = {-0.0002f, 3.0f, -2e-5f, 3.0f};
    o.time_pad = {t, 0, 0, 0};
    o.light_ambient = {1, 1, 1, 1};
    o.light0_vector = {0, 0, 1, 0};
    o.light0_diffuse = {1, 1, 1, 1};
    o.fog_color = {0, 0, 0, 1};
}

void store_rows(const Mat3x4& m, materials::float4 (&rows)[3]) {
    for (int i = 0; i < 3; ++i) rows[i] = {m.m[i][0], m.m[i][1], m.m[i][2], m.m[i][3]};
}

}  // namespace alamo::render
