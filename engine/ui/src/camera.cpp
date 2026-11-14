#include "alamo/ui/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::ui {

float tactical_fov_x(float fov_degrees, float aspect) {
    const float f = fov_degrees * kDegToRad;
    if (aspect <= 4.0f / 3.0f + 1e-4f) return f;
    return 2.0f * std::atan(std::tan(0.5f * f) * 0.75f * 16.0f / 9.0f);
}

float galactic_fov_x(float fov_x_4_3, float aspect) {
    if (aspect <= 4.0f / 3.0f + 1e-4f) return fov_x_4_3;
    return 2.0f * std::atan(std::tan(0.5f * fov_x_4_3) * aspect * 0.75f);
}

float Spline::eval(float t, float fallback) const {
    const size_t n = x.size() < y.size() ? x.size() : y.size();
    if (n == 0) return fallback;
    if (t <= x[0]) return y[0];
    for (size_t i = 1; i < n; ++i) {
        if (t <= x[i]) {
            const float span = x[i] - x[i - 1];
            const float u = span > 0.0f ? (t - x[i - 1]) / span : 1.0f;
            return y[i - 1] + (y[i] - y[i - 1]) * u;
        }
    }
    return y[n - 1];
}

std::optional<Vec3> focus_point(std::span<const Vec3> points) {
    if (points.empty()) return std::nullopt;
    double x = 0, y = 0, z = 0;
    for (const Vec3& p : points) {
        x += static_cast<double>(p.x);
        y += static_cast<double>(p.y);
        z += static_cast<double>(p.z);
    }
    const double n = static_cast<double>(points.size());
    return Vec3{static_cast<float>(x / n), static_cast<float>(y / n), static_cast<float>(z / n)};
}

// ---- constants -----------------------------------------------------------------------------------------
TacticalCameraConstants space_mode_defaults() { return {}; }

TacticalCameraConstants land_mode_defaults() {
    TacticalCameraConstants c;
    c.name = "Land_Mode";
    c.pitch_default = 50, c.pitch_min = 50, c.pitch_max = 50, c.pitch_per_mouse_unit = 0;
    c.pitch_per_zoom_unit = -30, c.pitch_when_zoomed_in = 15, c.pitch_zoom_begin_fraction = 0.1f;
    c.yaw_default = 0, c.yaw_min = -1000, c.yaw_max = 1000, c.yaw_per_mouse_unit = 1.5f;
    c.fov_default = 55, c.fov_min = 25, c.fov_max = 55, c.fov_smooth_time = 0.05f, c.fov_per_mouse_unit = 0;
    c.distance_default = 460, c.distance_min = 170, c.distance_max = 460, c.distance_smooth_time = 0.21f;
    c.distance_per_mouse_unit = 100;
    c.use_splines = true;
    c.pitch_spline = {{0.0f, 0.35f, 1.0f, 1.1f}, {27.0f, 45.0f, 55.0f, 55.0f}};
    c.distance_spline = {{0.0f, 0.3f, 1.0f}, {170.0f, 220.0f, 460.0f}};
    c.spline_steps = 15;
    c.overview_distance = 1000, c.overview_pitch = 65, c.overview_fov = 55, c.overview_clicks = 4;
    c.overview_click_time = 1.5f;
    c.overview_distance2 = 1600, c.overview_pitch2 = 80, c.overview_fov2 = 70;
    c.location_follows_terrain = true, c.min_height_above_terrain = 20;
    c.height_up_smooth_time = 0.3f, c.height_down_smooth_time = 1.0f;
    c.near_clip = 10, c.far_clip = 7000, c.fade_begin = 1100, c.fade_end = 1300;
    c.side_bounds_buffer = 0, c.top_bounds_buffer = 0, c.bottom_bounds_buffer = 0;
    return c;
}

TacticalCameraConstants unlocked_defaults() {
    TacticalCameraConstants c;
    c.name = "Unlocked";
    c.pitch_default = 34, c.pitch_min = 0, c.pitch_max = 80, c.pitch_per_mouse_unit = -1.5f;
    c.pitch_per_zoom_unit = 0, c.pitch_when_zoomed_in = 34, c.pitch_zoom_begin_fraction = -1;
    c.yaw_min = -360, c.yaw_max = 360;
    c.fov_default = 55, c.fov_min = 55, c.fov_max = 55, c.fov_smooth_time = 0.1f;
    c.distance_default = 900, c.distance_min = 0, c.distance_max = 60000, c.distance_per_mouse_unit = 50;
    c.overview_distance = 2500, c.overview_clicks = 99, c.overview_click_time = 0;
    c.near_clip = 1, c.far_clip = 60000, c.fade_begin = 10000, c.fade_end = 11000;
    c.side_bounds_buffer = 0, c.top_bounds_buffer = 0, c.bottom_bounds_buffer = 0;
    return c;
}

namespace {
void read_f(const data::XmlNode& n, std::string_view tag, float& v) {
    const data::XmlNode c = n.child(tag);
    if (c) v = data::parse_float_or(c.text_trimmed(), v);
}
void read_i(const data::XmlNode& n, std::string_view tag, int32_t& v) {
    const data::XmlNode c = n.child(tag);
    if (c) v = data::parse_int_or(c.text_trimmed(), v);
}
void read_b(const data::XmlNode& n, std::string_view tag, bool& v) {
    const data::XmlNode c = n.child(tag);
    if (!c) return;
    if (auto b = data::parse_bool(c.text_trimmed())) {
        v = *b;
    } else if (auto i = data::parse_int(c.text_trimmed())) {
        v = *i != 0;
    }
}
void read_spline(const data::XmlNode& n, std::string_view tag, Spline& s) {
    const data::XmlNode c = n.child(tag);
    if (!c) return;
    float buf[64];
    const size_t k = data::parse_floats(c.text_trimmed(), buf, 64) / 2;
    s.x.clear();
    s.y.clear();
    for (size_t i = 0; i < k; ++i) {
        s.x.push_back(buf[2 * i]);
        s.y.push_back(buf[2 * i + 1]);
    }
}
void read_v3(const data::XmlNode& n, std::string_view tag, Vec3& v) {
    const data::XmlNode c = n.child(tag);
    if (!c) return;
    float b[3];
    if (data::parse_floats(c.text_trimmed(), b, 3) == 3) v = {b[0], b[1], b[2]};
}
}  // namespace

size_t parse_tactical_cameras(const data::XmlNode& root, std::vector<TacticalCameraConstants>& out) {
    size_t count = 0;
    for (data::XmlNode n : root.children("TacticalCamera")) {
        TacticalCameraConstants c;
        c.name = std::string(n.attribute("Name"));
        read_f(n, "Pitch_Default", c.pitch_default);
        read_f(n, "Pitch_Min", c.pitch_min);
        read_f(n, "Pitch_Max", c.pitch_max);
        read_f(n, "Pitch_Per_Mouse_Unit", c.pitch_per_mouse_unit);
        read_f(n, "Pitch_Per_Zoom_Unit", c.pitch_per_zoom_unit);
        read_f(n, "Pitch_When_Zoomed_In", c.pitch_when_zoomed_in);
        read_f(n, "Pitch_Zoom_Begin_Fraction", c.pitch_zoom_begin_fraction);
        read_f(n, "Yaw_Default", c.yaw_default);
        read_f(n, "Yaw_Min", c.yaw_min);
        read_f(n, "Yaw_Max", c.yaw_max);
        read_f(n, "Yaw_Per_Mouse_Unit", c.yaw_per_mouse_unit);
        read_f(n, "Fov_Default", c.fov_default);
        read_f(n, "Fov_Min", c.fov_min);
        read_f(n, "Fov_Max", c.fov_max);
        read_f(n, "Fov_Smooth_Time", c.fov_smooth_time);
        read_f(n, "Fov_Per_Mouse_Unit", c.fov_per_mouse_unit);
        read_f(n, "Distance_Default", c.distance_default);
        read_f(n, "Distance_Min", c.distance_min);
        read_f(n, "Distance_Max", c.distance_max);
        read_f(n, "Distance_Smooth_Time", c.distance_smooth_time);
        read_f(n, "Distance_Per_Mouse_Unit", c.distance_per_mouse_unit);
        read_b(n, "Use_Splines", c.use_splines);
        read_spline(n, "Pitch_Spline", c.pitch_spline);
        read_spline(n, "Distance_Spline", c.distance_spline);
        read_i(n, "Spline_Steps", c.spline_steps);
        read_f(n, "Spline_Smooth_Time", c.spline_smooth_time);
        read_f(n, "Tactical_Overview_Distance", c.overview_distance);
        read_f(n, "Tactical_Overview_Pitch", c.overview_pitch);
        read_f(n, "Tactical_Overview_FOV", c.overview_fov);
        read_i(n, "Tactical_Overview_Clicks", c.overview_clicks);
        read_f(n, "Tactical_Overview_Click_Time", c.overview_click_time);
        read_f(n, "Tactical_Overview_Distance2", c.overview_distance2);
        read_f(n, "Tactical_Overview_Pitch2", c.overview_pitch2);
        read_f(n, "Tactical_Overview_FOV2", c.overview_fov2);
        read_b(n, "Location_Follows_Terrain", c.location_follows_terrain);
        read_f(n, "Min_Height_Above_Terrain", c.min_height_above_terrain);
        read_f(n, "MinHeightAboveTerrain", c.min_height_above_terrain);
        read_f(n, "Location_Height_Up_Smooth_Time", c.height_up_smooth_time);
        read_f(n, "Location_Height_Down_Smooth_Time", c.height_down_smooth_time);
        read_f(n, "Near_Clip", c.near_clip);
        read_f(n, "Far_Clip", c.far_clip);
        read_f(n, "Object_Fade_Begin", c.fade_begin);
        read_f(n, "Object_Fade_End", c.fade_end);
        read_f(n, "Side_Bounds_Buffer", c.side_bounds_buffer);
        read_f(n, "Top_Bounds_Buffer", c.top_bounds_buffer);
        read_f(n, "Bottom_Bounds_Buffer", c.bottom_bounds_buffer);
        out.push_back(std::move(c));
        ++count;
    }
    return count;
}

const TacticalCameraConstants* find_camera(const std::vector<TacticalCameraConstants>& v, std::string_view name) {
    for (const TacticalCameraConstants& c : v) {
        if (c.name.size() != name.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < name.size() && eq; ++i) {
            const auto lo = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
            eq = lo(c.name[i]) == lo(name[i]);
        }
        if (eq) return &c;
    }
    return nullptr;
}

UiConstants parse_ui_constants(const data::XmlNode& g) {
    UiConstants u;
    read_i(g, "Tactical_Edge_Scroll_Region", u.tactical.edge_region_px);
    read_f(g, "Tactical_Max_Scroll_Speed", u.tactical.max_speed);
    read_f(g, "Tactical_Min_Scroll_Speed", u.tactical.min_speed);
    read_i(g, "Tactical_Offscreen_Scroll_Region", u.tactical.offscreen_region_px);
    read_i(g, "Strategic_Edge_Scroll_Region", u.strategic.edge_region_px);
    read_f(g, "Strategic_Max_Scroll_Speed", u.strategic.max_speed);
    read_f(g, "Strategic_Min_Scroll_Speed", u.strategic.min_speed);
    read_i(g, "Strategic_Offscreen_Scroll_Region", u.strategic.offscreen_region_px);
    float push = u.tactical.push_speed_modifier;
    read_f(g, "Push_Scroll_Speed_Modifier", push);
    u.tactical.push_speed_modifier = u.strategic.push_speed_modifier = push;
    float decel = u.tactical.deceleration, accel = u.tactical.acceleration;
    read_f(g, "Scroll_Deceleration_Factor", decel);
    read_f(g, "Scroll_Acceleration_Factor", accel);
    u.tactical.deceleration = u.strategic.deceleration = decel;
    u.tactical.acceleration = u.strategic.acceleration = accel;
    GalacticCameraConstants& c = u.galactic;
    read_f(g, "GMC_InitialPitchAngleDegrees", c.initial_pitch_deg);
    read_f(g, "GMC_ZoomedPitchAngleDegrees", c.zoomed_pitch_deg);
    read_f(g, "GMC_InitialPullbackDistance", c.initial_pullback);
    read_f(g, "GMC_ZoomedPullbackPlanetRadiusFraction", c.zoomed_pullback_radius_fraction);
    read_v3(g, "GMC_ZoomedPositionOffsetPlanetRadiusFractions", c.zoomed_offset_radius_fractions);
    read_f(g, "GMC_ZoomTime", c.zoom_time);
    read_f(g, "Galactic_Right_Button_Scroll_Speed_Factor", c.right_button_scroll_factor);
    read_f(g, "Galactic_Scroll_Plane", c.scroll_plane);
    read_v3(g, "Starting_Galactic_Camera_Position", c.start_position);
    read_f(g, "MinimumDragDistance", u.minimum_drag_distance);
    read_f(g, "MinimumDragSelectDistance", u.minimum_drag_select_distance);
    return u;
}

// ---- shared helpers -------------------------------------------------------------------------------------
namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Angle in degrees wrapped to (-180, 180] [port: Gabriel free rotation].
float wrap180(float a) {
    a = std::fmod(a, 360.0f);
    if (a > 180.0f) a -= 360.0f;
    if (a <= -180.0f) a += 360.0f;
    return a;
}
// Pose -> params (SPACE_AND_OPEN D.7 2).
CameraParams make_params(Vec3 look, float yaw_deg, float pitch_deg, float distance) {
    CameraParams p;
    const float y = yaw_deg * kDegToRad, pt = pitch_deg * kDegToRad;
    const float fx = std::sin(y), fy = std::cos(y);
    const float cp = std::cos(pt), sp = std::sin(pt);
    p.target = look;
    p.eye = {look.x - distance * cp * fx, look.y - distance * cp * fy, look.z + distance * sp};
    p.orbit_yaw = -0.5f * kPi - y;
    p.orbit_pitch = pt;
    p.distance = distance;
    return p;
}
float key_axis(const InputState& in, const Bindings& k, Action neg, Action pos) {
    return (k.held(in, pos) ? 1.0f : 0.0f) - (k.held(in, neg) ? 1.0f : 0.0f);
}
int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Scroll target velocity T (P1_CAMERA_INPUT.md §2.1), speed units / s, y screen-down.
//   edge: edge scrolling allowed this frame; push: push mode (distances from the press point (px, py) past a dead zone).
void scroll_target(const InputState& in, const Bindings& keys, const ScrollConstants& s, const CameraTuning& t, bool edge,
                   bool push, int32_t px, int32_t py, float& tx, float& ty) {
    const InputFrame& f = in.frame();
    const int32_t off = s.offscreen_region_px > 0 ? s.offscreen_region_px : 1;
    int32_t dr = 0, dl = 0, db = 0, dtop = 0;
    if (edge) {
        const int32_t x = f.mouse_x, y = f.mouse_y, e = s.edge_region_px;
        if (push) {
            const int32_t dz = t.push_dead_zone_px;
            dr = x - px - dz, dl = px - x - dz, db = y - py - dz, dtop = py - y - dz;
        } else {
            const int32_t w = static_cast<int32_t>(f.width), h = static_cast<int32_t>(f.height);
            dr = x - (w - e - 1), dl = e - x, db = y - (h - e - 1), dtop = e - y;
        }
        dr = clampi(dr, 0, off), dl = clampi(dl, 0, off), db = clampi(db, 0, off), dtop = clampi(dtop, 0, off);
    }
    if (keys.held(in, Action::ScrollRight)) dr = off;
    if (keys.held(in, Action::ScrollLeft)) dl = off;
    if (keys.held(in, Action::ScrollDown)) db = off;
    if (keys.held(in, Action::ScrollUp)) dtop = off;
    const auto speed = [&](int32_t d) {
        if (d <= 0) return 0.0f;
        if (!t.windowed) return s.min_speed + 0.5f * (s.max_speed - s.min_speed);
        return s.min_speed + (static_cast<float>(d) / static_cast<float>(off)) * (s.max_speed - s.min_speed);
    };
    const float opt = t.scroll_speed_option > 0.1f ? t.scroll_speed_option : 0.1f;
    const float m = opt * (push ? s.push_speed_modifier : 1.0f);
    tx = (speed(dr) - speed(dl)) * m;
    ty = (speed(db) - speed(dtop)) * m * 1.2f;
}
}  // namespace

void scroll_smooth(float tx, float ty, float dt, const ScrollConstants& s, float& vx, float& vy) {
    const float n = dt * s.reference_rate;  // updates of the original in this frame
    if (!(n > 0.0f)) return;
    if (tx == 0.0f && ty == 0.0f) {
        if (vx == 0.0f && vy == 0.0f) return;
        const float k = s.deceleration > 1.0f ? std::pow(1.0f / s.deceleration, n) : 0.0f;  // [port] <= 1: stop at once
        vx *= k;
        vy *= k;
        const float stop = 0.1f * s.reference_rate;  // 0.1 speed units per update
        if (std::fabs(vx) < stop && std::fabs(vy) < stop) vx = vy = 0.0f;
        return;
    }
    // Per component, clamped so it never passes the target; a zero target component is cut at once.
    auto acc = [&](float& v, float t) {
        if (t == 0.0f) {
            v = 0.0f;
            return;
        }
        if (!(s.acceleration > 0.0f)) {  // [port] no acceleration: the target at once
            v = t;
            return;
        }
        const float next = v + s.acceleration * t * n;
        v = t > 0.0f ? std::min(next, t) : std::max(next, t);
    };
    acc(vx, tx);
    acc(vy, ty);
}

void smooth_cd(float& value, float& velocity, float target, float smooth_time, float dt) {
    if (!(smooth_time > 0.0f)) {
        value = target;
        velocity = 0.0f;
        return;
    }
    if (!(dt > 0.0f)) return;
    const float omega = 2.0f / smooth_time;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float c = value - target;
    const float tmp = (velocity + omega * c) * dt;
    velocity = (velocity - omega * tmp) * e;
    value = target + (c + tmp) * e;
}

CameraBox galactic_scroll_box(std::span<const Vec3> centres, std::span<const float> half_extents) {
    if (centres.empty()) return {0, 0, 0, 0};
    CameraBox b{1e30f, 1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < centres.size(); ++i) {
        const float h = i < half_extents.size() ? half_extents[i] : 0.0f;
        b.min_x = std::min(b.min_x, centres[i].x - h);
        b.min_y = std::min(b.min_y, centres[i].y - h);
        b.max_x = std::max(b.max_x, centres[i].x + h);
        b.max_y = std::max(b.max_y, centres[i].y + h);
    }
    return b;
}

// ---- tactical -------------------------------------------------------------------------------------------
TacticalCameraController::TacticalCameraController(TacticalCameraConstants c, ScrollConstants s, CameraTuning t)
    : c_(std::move(c)), s_(s), t_(t) {
    reset();
}

void TacticalCameraController::set_bounds(const CameraBox& b) {
    box_ = b;
    set_look_at(look_);
}

CameraBox TacticalCameraController::limits() const {
    CameraBox l{box_.min_x + c_.side_bounds_buffer, box_.min_y + c_.bottom_bounds_buffer,
                box_.max_x - c_.side_bounds_buffer, box_.max_y - c_.top_bounds_buffer};
    if (l.min_x > l.max_x) l.min_x = l.max_x = 0.5f * (box_.min_x + box_.max_x);
    if (l.min_y > l.max_y) l.min_y = l.max_y = 0.5f * (box_.min_y + box_.max_y);
    return l;
}

void TacticalCameraController::reset() {
    yaw_ = clampf(c_.yaw_default, c_.yaw_min, c_.yaw_max);
    pitch_ = clampf(c_.pitch_default, c_.pitch_min, c_.pitch_max);
    dist_.snap(clampf(c_.distance_default, c_.distance_min, c_.distance_max));
    fov_.snap(clampf(c_.fov_default, c_.fov_min, c_.fov_max));
    spl_.snap(1.0f);  // zoomed out (0x2cf800)
    ov_level_ = 0;
    ov_t_ = 1.0f;
    ov_clicks_ = 0.0f;
    pitch_free_ = roll_ = 0.0f;
}

void TacticalCameraController::set_look_at(Vec3 p) {
    const CameraBox l = limits();
    look_ = {clampf(p.x, l.min_x, l.max_x), clampf(p.y, l.min_y, l.max_y), p.z};
}

void TacticalCameraController::set_terrain_height(TerrainHeightFn fn) {
    height_ = std::move(fn);
    if (height_ && c_.location_follows_terrain) {
        look_.z = height_(look_.x, look_.y);
        height_s_.snap(look_.z);
    }
}

void TacticalCameraController::jump_to(Vec3 p) {
    set_look_at(p);
    if (height_ && c_.location_follows_terrain) {
        look_.z = height_(look_.x, look_.y);
        height_s_.snap(look_.z);
    }
}

// ---- overview ----
int32_t TacticalCameraController::max_overview_level() const {
    if (!(c_.overview_distance > 0.0f)) return 0;
    return c_.overview_distance2 > c_.overview_distance ? 2 : 1;
}

TacticalCameraController::Pose TacticalCameraController::normal_pose() const {
    float pitch = pitch_, dist = dist_.value;
    if (c_.use_splines) {  // §4.3: in force from the first frame
        pitch = c_.pitch_spline.eval(spl_.value, pitch_);
        dist = c_.distance_spline.eval(spl_.value, dist_.value);
    } else {
        const float b = c_.pitch_zoom_begin_fraction, f = zoom_fraction();
        if (b > 0.0f && f < b) pitch = pitch_ + (c_.pitch_when_zoomed_in - pitch_) * (b - f) / b;  // local only
    }
    return {dist, pitch_free_ != 0.0f ? wrap180(pitch + pitch_free_) : pitch, fov_override_ > 0.0f ? fov_override_ : fov_.value};
}

TacticalCameraController::Pose TacticalCameraController::level_pose(int32_t level) const {
    const Pose n = normal_pose();
    if (level <= 0) return n;
    const bool two = level >= 2;
    const float d = two ? c_.overview_distance2 : c_.overview_distance;
    const float p = two ? c_.overview_pitch2 : c_.overview_pitch;
    const float f = two ? c_.overview_fov2 : c_.overview_fov;
    return {d, p < 0.0f ? n.pitch : clampf(p, -89.0f, 89.0f), f > 0.0f ? f : n.fov};
}

TacticalCameraController::Pose TacticalCameraController::effective_pose() const {
    const Pose to = level_pose(ov_level_);
    if (ov_t_ >= 1.0f) return to;
    const float s = ov_t_ * ov_t_ * (3.0f - 2.0f * ov_t_);
    const auto lerp = [s](float a, float b) { return a + (b - a) * s; };
    return {lerp(ov_from_.distance, to.distance), lerp(ov_from_.pitch, to.pitch), lerp(ov_from_.fov, to.fov)};
}

void TacticalCameraController::set_overview(int32_t level) {
    level = level < 0 ? 0 : (level > max_overview_level() ? max_overview_level() : level);
    if (level == ov_level_) return;
    ov_from_ = effective_pose();
    ov_level_ = level;
    ov_t_ = t_.overview_blend_time > 0.0f ? 0.0f : 1.0f;
    ov_clicks_ = 0.0f;
}

void TacticalCameraController::toggle_overview() {
    const int32_t m = max_overview_level();
    set_overview(ov_level_ >= m ? 0 : ov_level_ + 1);
}

void TacticalCameraController::overview_click(float notches) {
    if (c_.overview_clicks <= 0 || ov_level_ >= max_overview_level()) return;
    if (ov_clicks_ <= 0.0f || time_ - ov_click_start_ > static_cast<double>(c_.overview_click_time)) {
        ov_clicks_ = 0.0f;
        ov_click_start_ = time_;
    }
    ov_clicks_ += notches;
    if (ov_clicks_ >= static_cast<float>(c_.overview_clicks) - 1e-4f) set_overview(ov_level_ + 1);
}
void TacticalCameraController::set_yaw_deg(float y) { yaw_ = t_.free_rotation ? wrap180(y) : clampf(y, c_.yaw_min, c_.yaw_max); }
void TacticalCameraController::set_pitch_deg(float p) { pitch_ = t_.free_rotation ? wrap180(p) : clampf(p, c_.pitch_min, c_.pitch_max); }
void TacticalCameraController::set_distance(float d) {
    dist_.snap(clampf(d, c_.distance_min, c_.distance_max));
    const float span = c_.distance_max - c_.distance_min;
    spl_.snap(span > 0.0f ? clampf((dist_.value - c_.distance_min) / span, 0.0f, 1.0f) : 1.0f);  // [port]
}

void TacticalCameraController::pan(float right, float forward) {
    const float y = yaw_ * kDegToRad;
    const float fx = std::sin(y), fy = std::cos(y);  // forward; right = (fy, -fx)
    set_look_at({look_.x + right * fy + forward * fx, look_.y - right * fx + forward * fy, look_.z});
}

void TacticalCameraController::rotate(float dyaw, float dpitch) {
    if (t_.free_rotation) {  // [port: Gabriel] no clamps; the pitch goes on top of the data / spline pitch
        yaw_ = wrap180(yaw_ + dyaw);
        pitch_free_ = wrap180(pitch_free_ + dpitch);
        return;
    }
    yaw_ = clampf(wrap180(yaw_ + dyaw), c_.yaw_min, c_.yaw_max);
    pitch_ = clampf(pitch_ + dpitch, c_.pitch_min, c_.pitch_max);
}

void TacticalCameraController::roll(float droll) {
    if (t_.free_rotation) roll_ = wrap180(roll_ + droll);
}

void TacticalCameraController::zoom(float notches) {
    if (notches == 0.0f) return;
    if (ov_level_ > 0) {  // overview: in -> back to the normal pose, out -> towards the next level
        if (notches > 0.0f) set_overview(0);
        else overview_click(-notches);
        return;
    }
    const bool at_max = dist_.target >= c_.distance_max - 1e-3f;
    dist_.target = clampf(dist_.target - notches * c_.distance_per_mouse_unit, c_.distance_min, c_.distance_max);
    fov_.target = clampf(fov_.target - notches * c_.fov_per_mouse_unit, c_.fov_min, c_.fov_max);
    if (c_.spline_steps > 0) spl_.target = clampf(spl_.target - notches / static_cast<float>(c_.spline_steps), 0.0f, 1.0f);
    if (notches < 0.0f && at_max) overview_click(-notches);
}

void TacticalCameraController::step(float dt) {
    time_ += static_cast<double>(dt);
    smooth_cd(dist_.value, dist_.vel, dist_.target, c_.distance_smooth_time, dt);
    smooth_cd(fov_.value, fov_.vel, fov_.target, c_.fov_smooth_time, dt);
    smooth_cd(spl_.value, spl_.vel, spl_.target, c_.spline_smooth_time, dt);
    if (ov_t_ < 1.0f)
        ov_t_ = t_.overview_blend_time > 0.0f ? clampf(ov_t_ + dt / t_.overview_blend_time, 0.0f, 1.0f) : 1.0f;
    if (height_ && c_.location_follows_terrain) {
        height_s_.target = height_(look_.x, look_.y);
        const float st = height_s_.target > height_s_.value ? c_.height_up_smooth_time : c_.height_down_smooth_time;
        smooth_cd(height_s_.value, height_s_.vel, height_s_.target, st, dt);
        look_.z = height_s_.value;
    }
}

float TacticalCameraController::zoom_fraction() const {
    if (c_.use_splines) return spl_.value;
    if (!(c_.distance_per_mouse_unit > 0.0f)) {  // FOV zoom
        const float fs = c_.fov_max - c_.fov_min;
        return fs > 0.0f ? clampf((fov_.value - c_.fov_min) / fs, 0.0f, 1.0f) : 1.0f;
    }
    const float span = c_.distance_max - c_.distance_min;
    return span > 0.0f ? clampf((dist_.value - c_.distance_min) / span, 0.0f, 1.0f) : 1.0f;
}
float TacticalCameraController::pitch_deg() const { return effective_pose().pitch; }
float TacticalCameraController::distance() const { return effective_pose().distance; }
float TacticalCameraController::fov_deg() const { return effective_pose().fov; }
float TacticalCameraController::scroll_speed() const {
    const float opt = t_.scroll_speed_option > 0.1f ? t_.scroll_speed_option : 0.1f;
    return s_.key_speed(t_.windowed) * opt * 3e-4f * dist_.value;
}

// Mouse camera modes (§3): middle drag pans (Ctrl: rotates), right drag with IDC_RIGHT_BUTTON_SCROLL, both buttons rotate.
void TacticalCameraController::mouse(const InputState& in) {
    const ButtonState& mb = in.button(Button::Middle);
    const ButtonState& rb = in.button(Button::Right);
    const ButtonState& lb = in.button(Button::Left);
    const InputFrame& f = in.frame();
    if (mb.pressed) drag_moved_ = false;
    if (rb.pressed) {
        right_acc_ = 0.0f;
        right_active_ = false;
    }
    const float w = static_cast<float>(f.width > 0 ? f.width : 1), h = static_cast<float>(f.height > 0 ? f.height : 1);
    if (t_.right_button_scroll && rb.down && !right_active_) {
        right_acc_ += 4.0f * (static_cast<float>(std::abs(rb.dx)) / w + static_cast<float>(std::abs(rb.dy)) / h);
        right_active_ = right_acc_ > 0.08f;
    }
    drag_ = Drag::None;
    int32_t dx = 0, dy = 0;
    float fmul = 4.0f;
    if (t_.both_buttons_rotate && lb.down && rb.down) {
        drag_ = Drag::Rotate;
        dx = rb.dx, dy = rb.dy;
    } else if (mb.down || (t_.right_button_scroll && rb.down && right_active_)) {
        const ButtonState& b = mb.down ? mb : rb;
        drag_ = in.ctrl() ? Drag::Rotate : Drag::Pan;
        fmul = in.ctrl() ? 1.0f : 4.0f;
        dx = b.dx, dy = b.dy;
        if (mb.down && (dx != 0 || dy != 0)) drag_moved_ = true;
    }
    if (mb.released) {
        // A middle click without movement resets the camera (look-at kept); not with Ctrl.
        if (!in.ctrl() && !drag_moved_ && mb.drag.x0 == mb.drag.x1 && mb.drag.y0 == mb.drag.y1) reset();
        drag_moved_ = false;
    }
    if (dx == 0 && dy == 0) return;
    const float mux = 100.0f * fmul * static_cast<float>(dx) / w;
    const float muy = -100.0f * fmul * static_cast<float>(dy) / h;  // mouse up > 0
    if (drag_ == Drag::Pan) {
        if (t_.absolute_scrolling) pan(0.01f * dist_.value * mux, 0.01f * dist_.value * muy);  // else push-scroll
    } else if (drag_ == Drag::Rotate && ov_level_ == 0) {
        const float ppu = t_.free_rotation && c_.pitch_per_mouse_unit == 0.0f ? t_.free_pitch_per_mouse_unit : c_.pitch_per_mouse_unit;
        rotate(mux * c_.yaw_per_mouse_unit, muy * ppu);
    }
}

void TacticalCameraController::update(const InputState& in, const Bindings& keys, float dt) {
    if (dt > 1.0f) dt = 1.0f;
    mouse(in);
    // scroll: keys + screen edge / push (§2)
    const ButtonState& lb = in.button(Button::Left);
    const bool dragging = drag_ != Drag::None;
    const bool push = !t_.absolute_scrolling && drag_ == Drag::Pan;
    const bool edge = drag_ != Drag::Rotate && !(t_.absolute_scrolling && dragging) && !lb.down &&
                      (in.frame().mouse_inside || dragging);
    const ButtonState& db = in.button(in.button(Button::Middle).down ? Button::Middle : Button::Right);
    float tx = 0, ty = 0;
    scroll_target(in, keys, s_, t_, edge, push, db.drag.x0, db.drag.y0, tx, ty);
    scroll_smooth(tx, ty, dt, s_, scroll_vx_, scroll_vy_);
    if (scroll_vx_ != 0.0f || scroll_vy_ != 0.0f) {
        const float k = 3e-4f * dist_.value * dt;  // world speed = S x 3e-4 x distance
        pan(scroll_vx_ * k, -scroll_vy_ * k);
    }
    // [port: Gabriel] keyboard yaw / pitch / roll
    const float dyaw = key_axis(in, keys, Action::RotateLeft, Action::RotateRight) * t_.key_yaw_deg_per_s * dt;
    float dpitch = 0.0f;
    if (t_.free_rotation) {
        dpitch = key_axis(in, keys, Action::PitchDown, Action::PitchUp) * t_.key_pitch_deg_per_s * dt;
        const float dr = key_axis(in, keys, Action::RollLeft, Action::RollRight) * t_.key_roll_deg_per_s * dt;
        if (dr != 0.0f) roll(dr);
    }
    if (dyaw != 0.0f || dpitch != 0.0f) rotate(dyaw, dpitch);
    // zoom: wheel (not while a mouse drag is active) + [port: Gabriel] keys
    const float zk = key_axis(in, keys, Action::ZoomOut, Action::ZoomIn) * t_.key_zoom_notches_per_s * dt;
    zoom((dragging ? 0.0f : in.wheel()) + zk);
    if (keys.pressed(in, Action::ResetCamera)) reset();
    step(dt);
}

CameraParams TacticalCameraController::params(float aspect) const {
    const Pose e = effective_pose();
    float pitch = e.pitch;
    if (height_ && c_.min_height_above_terrain > 0.0f && e.distance > 0.0f) {
        // [port] Raise the pitch until the eye clears the terrain under it (the eye moves as the pitch changes).
        for (int it = 0; it < 8; ++it) {
            const CameraParams q = make_params(look_, yaw_, pitch, e.distance);
            const float floor_z = height_(q.eye.x, q.eye.y) + c_.min_height_above_terrain;
            if (q.eye.z >= floor_z - 1e-3f) break;
            const float s = (floor_z - look_.z) / e.distance;
            const float need = s >= 0.999f ? 88.0f : std::asin(clampf(s, -1.0f, 1.0f)) / kDegToRad + 0.01f;
            if (need <= pitch) break;
            pitch = need;
        }
    }
    CameraParams p = make_params(look_, yaw_, pitch, e.distance);
    p.fov_x = tactical_fov_x(e.fov, aspect);
    // [port: Gabriel] up vector of a rolled camera or one pitched past the vertical (orbit up = sin p * forward + cos p * Z,
    // perpendicular to the view, then rotated by the roll about the view direction).
    const float pr = pitch * kDegToRad;
    if (roll_ != 0.0f || std::cos(pr) < 1e-3f) {
        const float yr = yaw_ * kDegToRad;
        const Vec3 fh{std::sin(yr), std::cos(yr), 0.0f};
        const Vec3 u0{std::sin(pr) * fh.x, std::sin(pr) * fh.y, std::cos(pr)};
        Vec3 v{p.target.x - p.eye.x, p.target.y - p.eye.y, p.target.z - p.eye.z};
        const float vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (vl > 0.0f) v = {v.x / vl, v.y / vl, v.z / vl};
        const Vec3 r{v.y * u0.z - v.z * u0.y, v.z * u0.x - v.x * u0.z, v.x * u0.y - v.y * u0.x};
        const float rr = roll_ * kDegToRad, c = std::cos(rr), s = std::sin(rr);
        p.up = {u0.x * c + r.x * s, u0.y * c + r.y * s, u0.z * c + r.z * s};
        p.roll = rr;
    }
    p.znear = c_.near_clip;
    p.zfar = c_.far_clip;
    p.fade_begin = c_.fade_begin;
    p.fade_end = c_.fade_end;
    return p;
}

// ---- galactic -------------------------------------------------------------------------------------------
GalacticCameraController::GalacticCameraController(GalacticCameraConstants c, ScrollConstants s, CameraTuning t)
    : c_(c), s_(s), t_(t), look_{c.start_position.x, c.start_position.y, c.scroll_plane} {}

void GalacticCameraController::set_bounds(const CameraBox& b) {
    box_ = b;
    set_look_at(look_);
}
CameraBox GalacticCameraController::limits() const {
    const float cx = 0.5f * (box_.min_x + box_.max_x), cy = 0.5f * (box_.min_y + box_.max_y);
    const float hx = 0.5f * (box_.max_x - box_.min_x) * 1.15f, hy = 0.5f * (box_.max_y - box_.min_y) * 1.15f;
    return {cx - hx, cy - hy, cx + hx, cy + hy};
}
void GalacticCameraController::set_look_at(Vec3 p) {
    const CameraBox l = limits();
    look_ = {clampf(p.x, l.min_x, l.max_x), clampf(p.y, l.min_y, l.max_y), c_.scroll_plane};
}
void GalacticCameraController::pan(float dx, float dy) {
    if (zoom_state() != 0) return;  // the zoomed view is anchored on the planet
    set_look_at({look_.x + dx, look_.y + dy, look_.z});
}
void GalacticCameraController::zoom(float notches) {
    if (notches > 0.0f && zoom_state() == 0) zoom_in_request_ = true;
    if (notches < 0.0f && zoom_state() == 3) zoom_out();
}
void GalacticCameraController::zoom_to_planet(Vec3 planet, float radius) {
    from_ = blended();
    zoomed_ = true;
    planet_ = planet;
    planet_radius_ = radius;
    t_anim_ = 0.0f;
}
void GalacticCameraController::zoom_out() {
    if (!zoomed_) return;
    from_ = blended();
    zoomed_ = false;
    t_anim_ = 0.0f;
}
void GalacticCameraController::step(float dt) {
    if (t_anim_ < 1.0f) t_anim_ = c_.zoom_time > 0.0f ? clampf(t_anim_ + dt / c_.zoom_time, 0.0f, 1.0f) : 1.0f;
}

GalacticCameraController::Pose GalacticCameraController::free_pose() const {
    return {look_, -c_.initial_pitch_deg, c_.initial_pullback};
}
GalacticCameraController::Pose GalacticCameraController::zoom_pose() const {
    const Vec3 o = c_.zoomed_offset_radius_fractions;
    const float r = planet_radius_;
    return {{planet_.x + o.x * r, planet_.y + o.y * r, planet_.z + o.z * r}, -c_.zoomed_pitch_deg,
            r * c_.zoomed_pullback_radius_fraction};
}
GalacticCameraController::Pose GalacticCameraController::blended() const {
    const Pose to = zoomed_ ? zoom_pose() : free_pose();
    if (t_anim_ >= 1.0f) return to;
    const float s = t_anim_ * t_anim_ * (3.0f - 2.0f * t_anim_);
    const auto lerp = [s](float a, float b) { return a + (b - a) * s; };
    return {{lerp(from_.look.x, to.look.x), lerp(from_.look.y, to.look.y), lerp(from_.look.z, to.look.z)},
            lerp(from_.pitch, to.pitch), lerp(from_.pullback, to.pullback)};
}
Vec3 GalacticCameraController::look_at() const { return blended().look; }
float GalacticCameraController::pitch_deg() const { return blended().pitch; }
float GalacticCameraController::pullback() const { return blended().pullback; }

void GalacticCameraController::update(const InputState& in, const Bindings& keys, float dt) {
    if (dt > 1.0f) dt = 1.0f;
    const ButtonState& mb = in.button(Button::Middle);
    const ButtonState& rb = in.button(Button::Right);
    const InputFrame& f = in.frame();
    // Mouse drag scroll (§3.5): middle drag, or a right drag with IDC_RIGHT_BUTTON_SCROLL (starts at once); not while
    // zoomed in.
    const ButtonState& b = mb.down ? mb : rb;
    dragging_ = (mb.down || (t_.right_button_scroll && rb.down)) && zoom_state() != 3;
    if (dragging_ && t_.absolute_scrolling && (b.dx != 0 || b.dy != 0)) {
        const float w = static_cast<float>(f.width > 0 ? f.width : 1), h = static_cast<float>(f.height > 0 ? f.height : 1);
        const float fmul = in.ctrl() ? 1.0f : 4.0f;
        const float k = c_.right_button_scroll_factor * 1000.0f;
        pan(k * fmul * static_cast<float>(b.dx) / w, -k * fmul * static_cast<float>(b.dy) / h);
    }
    // Keys + screen edge / push (§2): S world units / s.
    const bool push = !t_.absolute_scrolling && dragging_;
    const bool edge = !(t_.absolute_scrolling && dragging_) && !in.button(Button::Left).down && (f.mouse_inside || dragging_);
    float tx = 0, ty = 0;
    scroll_target(in, keys, s_, t_, edge, push, b.drag.x0, b.drag.y0, tx, ty);
    scroll_smooth(tx, ty, dt, s_, scroll_vx_, scroll_vy_);
    if (scroll_vx_ != 0.0f || scroll_vy_ != 0.0f) pan(scroll_vx_ * dt, -scroll_vy_ * dt);
    // Wheel = planet zoom (§5); [port: Gabriel] the zoom keys act as one notch per press.
    float notches = dragging_ ? 0.0f : in.wheel();
    if (keys.pressed(in, Action::ZoomIn)) notches += 1.0f;
    if (keys.pressed(in, Action::ZoomOut)) notches -= 1.0f;
    zoom(notches);
    if (keys.pressed(in, Action::ResetCamera)) zoom_out();
    step(dt);
}

float GalacticCameraController::fov_deg() const { return fov_deg_ > 0.0f ? fov_deg_ : kGalacticFovX / kDegToRad; }

CameraParams GalacticCameraController::params(float aspect) const {
    const Pose p = blended();
    CameraParams cp = make_params(p.look, 0.0f, p.pitch, p.pullback);
    // B.8 keeps this horizontal FOV at every aspect; [port: Gabriel 2026-11-13] Hor+: it is the 4:3 value and wider windows
    // widen it (galactic_fov_x), so 16:9 / 21:9 / 32:9 keep 4:3's vertical view. [port: Gabriel] the FOV setting likewise.
    cp.fov_x = galactic_fov_x(fov_deg_ > 0.0f ? fov_deg_ * kDegToRad : kGalacticFovX, aspect);
    cp.znear = kGalacticNear;
    cp.zfar = kGalacticFar;
    cp.galactic = true;
    return cp;
}

}  // namespace alamo::ui
