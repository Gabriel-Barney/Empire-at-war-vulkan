// P1 camera controllers: tactical (Land_Mode / Space_Mode, TacticalCameras.xml) and galactic
// (GameConstants GMC_*). They turn InputState + Bindings into a camera pose and hand it out as
// CameraParams, which the renderer consumes (render_bridge.hpp -> render::OrbitCamera / ViewMatrices)
// and picking.hpp uses for screen rays. Camera state is local presentation state: it never feeds the
// sim (orders leave as commands, orders.hpp).
//
// Sources: docs/re/RENDER.md §3.4-3.5, SPACE_AND_OPEN.md §A.1-A.2, §B.8, §D.7 2 (pose), TacticalCameras.xml
// and GameConstants.xml (vanilla FoC values are the defaults below), xml_field_tables.md (field types).
//
// Pose (SPACE_AND_OPEN D.7 2, [H] order, [M] yaw axis): look-at point, yaw about +Z (0 = looking north,
// +Y; positive turns clockwise seen from above), pitch down from the horizontal (90 = straight down),
// eye = look_at + distance * (-cos(pitch) * forward + sin(pitch) * Z), forward = (sin yaw, cos yaw, 0).
// Yaw 0 / pitch 50 / distance 1000 puts the eye 766 above and 643 behind (-Y) the look-at point.
// FOV (degrees, defined for 4:3) is horizontal; displays wider than 4:3 get 2 atan(tan(f/2) * 0.75 * 16/9)
// (RENDER.md §3.4). Galactic: horizontal FOV atan(0.5), near 10, far 10000, no widening (B.8).
//
// Input, rates and smoothing follow docs/re/P1_CAMERA_INPUT.md (2026-10-28):
//   * scrolling (§2): per direction a distance d past the screen edge (keys: d = Offscreen_Region), speed S = Min + (d /
//     Offscreen) (Max - Min) windowed, the Min / Max midpoint fullscreen; T = (S_right - S_left, S_bottom - S_top) x the
//     options scroll-speed slider (>= 0.1), x Push_Scroll_Speed_Modifier in push mode, T.y x 1.2. The scroll velocity is
//     smoothed per update (Scroll_Acceleration / _Deceleration_Factor, ScrollConstants::reference_rate updates / s [port])
//     and applied as tactical world speed = S x 3e-4 x camera distance (the smoothed wheel distance), galactic S world u/s.
//   * mouse (§3): mouse units mu = 100 x 4 x pixels / screen size (x 1 with Ctrl). Middle drag pans 0.01 x distance x mu
//     (camera moves with the mouse); Ctrl + middle drag rotates (yaw += mu_x x Yaw_Per_Mouse_Unit, pitch += mu_y x
//     Pitch_Per_Mouse_Unit, unsmoothed); a middle click without movement resets the camera. Galactic: middle drag scrolls
//     Galactic_Right_Button_Scroll_Speed_Factor x 1000 x mouse units / 100. Options: right-button scroll, both-buttons
//     rotate, absolute scrolling off (= push scrolling while dragging) in CameraTuning.
//   * smoothing (§4.1): distance, FOV, spline fraction and terrain height use the critically damped SmoothCD spring with
//     the XML *_Smooth_Time (real dt); yaw / pitch / look-at are never smoothed.
//   * wheel (§4.2): per notch the distance / FOV / spline-fraction TARGETS move by Distance_Per_Mouse_Unit /
//     Fov_Per_Mouse_Unit / 1 / Spline_Steps. Splines are in force from the first frame (fraction starts at 1.0).
//   * bounds (§4.6): a positive *_Bounds_Buffer shrinks the look-at area, a negative one enlarges it.
//   * galactic (§1, §5): the look-at z is Galactic_Scroll_Plane; scrolling moves x / y only, clamped to the planet box
//     widened by 15 % of its half extents, and only while not zoomed. The wheel is the discrete planet zoom: wheel up
//     asks the host to zoom onto the hovered / selected planet (take_zoom_in_request), wheel down zooms out.
// [port] items (behaviour the docs do not give; all tunable in the structs): spline evaluation (piecewise linear over the
// XML knots), the scroll reference rate, the eye-above-terrain rule (pitch raised, see params()).
// [port: Gabriel] (docs/DECISIONS.md "Camera and controls") kept over the original: keyboard yaw / pitch / roll / zoom
// (the original has none), free 360 degree rotation, W A S D scrolling with any modifier, the FOV settings.
//
// P1 completion (2026-10-27), all [port] (xml_field_tables.md gives the fields, the docs give no behaviour):
//   * Tactical overview (Tactical_Overview_*): two overview levels above the normal zoom range. Zooming out with the
//     target distance already at Distance_Max counts notches; Tactical_Overview_Clicks of them within
//     Tactical_Overview_Click_Time seconds go up one level (level 2 only when Distance2 > Distance). The overview pose
//     is (Tactical_Overview_Distance[2], _Pitch[2] (< 0 = keep the current pitch), _FOV[2]); any zoom-in notch goes
//     back to the normal pose. Hotkey slot 76 (Insert) toggles 0 -> 1 -> 2 -> 0 (toggle_overview). Changes blend with
//     smoothstep over CameraTuning::overview_blend_time; splines stay in force underneath.
//   * Terrain following (Location_Follows_Terrain, Min_Height_Above_Terrain): with a height callback the look-at z
//     follows the terrain under it (linear smoothing over CameraTuning::terrain_smooth_time; snapped on jumps), and
//     the eye is kept Min_Height_Above_Terrain above the terrain under it by raising the pitch (so CameraParams and
//     the orbit pose stay one consistent camera).
//   * Control-group camera jump (UI.md §4.4): jump_to(point) moves the look-at point (clamped, terrain-snapped) and
//     keeps yaw / pitch / zoom; focus_point() = mean of the group's positions in index order. The host gets the jump
//     from HotkeyDispatcher (HotkeyEvent::camera_jump) or OrderTranslator::take_camera_jump().
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/tactical/geometry.hpp"
#include "alamo/ui/input.hpp"

namespace alamo::data {
class XmlNode;
}

namespace alamo::ui {

using tactical::Vec3;

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;

// What the renderer and picking need. Angles in radians.
struct CameraParams {
    Vec3 eye, target;
    float fov_x = 0.9599311f;  // horizontal FOV for the aspect the params were made for
    float znear = 10.0f, zfar = 7000.0f;
    float fade_begin = -1.0f, fade_end = -1.0f;  // Object_Fade_Begin / End (-1 = off)
    // render::OrbitCamera convention: yaw about +Z with 0 = camera on +X; pitch above the XY plane.
    float orbit_yaw = 0.0f, orbit_pitch = 0.0f, distance = 1.0f;
    bool galactic = false;
    // [port: Gabriel] free tactical camera: the camera's up vector (world +Z unless rolled or pitched past the vertical)
    // and its roll (radians). picking.hpp make_view and render_bridge.hpp use `up`; (0, 0, 1) = the orbit camera.
    Vec3 up{0, 0, 1};
    float roll = 0.0f;
};

// Hor+ rule of the tactical camera (RENDER.md §3.4); same formula as render::tactical_fov_x.
float tactical_fov_x(float fov_degrees, float aspect);
inline constexpr float kGalacticFovX = 0.46364761f;  // atan(0.5) (SPACE_AND_OPEN B.8)
inline constexpr float kGalacticNear = 10.0f, kGalacticFar = 10000.0f;

// Piecewise-linear curve over (x, y) knots, sorted by x; clamps outside. Empty -> fallback.
struct Spline {
    std::vector<float> x, y;
    bool empty() const { return x.empty(); }
    float eval(float t, float fallback) const;
};

// One <TacticalCamera Name="..."> block (TacticalCameras.xml). Defaults = vanilla Space_Mode.
struct TacticalCameraConstants {
    std::string name = "Space_Mode";
    float pitch_default = 50, pitch_min = -10, pitch_max = 85, pitch_per_mouse_unit = -1.5f;
    float pitch_per_zoom_unit = 0, pitch_when_zoomed_in = 50, pitch_zoom_begin_fraction = -1;
    float yaw_default = 0, yaw_min = -1000, yaw_max = 1000, yaw_per_mouse_unit = 1.5f;
    float fov_default = 55, fov_min = 55, fov_max = 55, fov_smooth_time = 0.1f, fov_per_mouse_unit = 0;
    float distance_default = 1000, distance_min = 200, distance_max = 1900, distance_smooth_time = 0.1f,
          distance_per_mouse_unit = 500;
    bool use_splines = false;
    Spline pitch_spline, distance_spline;  // x = zoom fraction 0 (in) .. 1 (out)
    int32_t spline_steps = 0;
    float overview_distance = 2200, overview_pitch = -1, overview_fov = 60;
    int32_t overview_clicks = 10;
    float overview_click_time = 1.8f;
    float overview_distance2 = 2900, overview_pitch2 = 80, overview_fov2 = 70;
    float spline_smooth_time = 0.1f;  // Spline_Smooth_Time (not in vanilla XML: record default [L])
    bool location_follows_terrain = false;
    float min_height_above_terrain = 0;
    float height_up_smooth_time = 0.1f, height_down_smooth_time = 0.1f;  // Location_Height_Up / Down_Smooth_Time
    float near_clip = 10, far_clip = 7000;
    float fade_begin = 6000, fade_end = 7000;
    float side_bounds_buffer = -300, top_bounds_buffer = -300, bottom_bounds_buffer = -300;
};
TacticalCameraConstants land_mode_defaults();    // vanilla FoC Land_Mode
TacticalCameraConstants space_mode_defaults();   // vanilla FoC Space_Mode
TacticalCameraConstants unlocked_defaults();     // vanilla FoC Unlocked
// Reads every <TacticalCamera> under `root` (missing fields keep the space defaults). Returns the count.
size_t parse_tactical_cameras(const data::XmlNode& root, std::vector<TacticalCameraConstants>& out);
const TacticalCameraConstants* find_camera(const std::vector<TacticalCameraConstants>& v, std::string_view name);

// GameConstants.xml fields the UI uses (defaults = vanilla FoC).
struct ScrollConstants {
    int32_t edge_region_px = 2;        // Tactical_/Strategic_Edge_Scroll_Region
    float max_speed = 4000, min_speed = 1000;  // *_Max / _Min_Scroll_Speed (speed units, see the header comment)
    int32_t offscreen_region_px = 50;  // *_Offscreen_Scroll_Region (distance past the edge for the full speed)
    float push_speed_modifier = 2.0f;  // Push_Scroll_Speed_Modifier (push mode only)
    // Scroll smoothing (P1_CAMERA_INPUT.md §2.2, STRATEGIC_GAPS §7.1), per-second form of the per-update rule: with
    // n = dt x reference_rate updates, T != 0 -> v += Scroll_Acceleration_Factor x T x n per component, clamped so it never
    // passes T (a zero component of T is cut to 0 at once); T = 0 -> v /= Scroll_Deceleration_Factor^n, v := 0 once
    // |vx| and |vy| < 0.1 x reference_rate (the original's 0.1 speed units per update). v and T are speeds (units / s).
    float acceleration = 0.08f;        // Scroll_Acceleration_Factor
    float deceleration = 1.5f;         // Scroll_Deceleration_Factor
    float reference_rate = 30.0f;      // [port] updates per second the per-update factors are defined for
    float key_speed(bool windowed) const { return windowed ? max_speed : min_speed + 0.5f * (max_speed - min_speed); }
};
// One smoothing step (above) of the scroll velocity (vx, vy) towards the target velocity (tx, ty) over `dt` seconds.
void scroll_smooth(float tx, float ty, float dt, const ScrollConstants& s, float& vx, float& vy);
// SmoothCD (Game Programming Gems 4; P1_CAMERA_INPUT.md §4.1): critically damped spring of `value` / `velocity` towards
// `target` with smooth time `smooth_time` over dt seconds; smooth_time <= 0 snaps.
void smooth_cd(float& value, float& velocity, float target, float smooth_time, float dt);
struct GalacticCameraConstants {
    float initial_pitch_deg = -30, zoomed_pitch_deg = -35;  // GMC_*PitchAngleDegrees (negative = down)
    float initial_pullback = 700, zoomed_pullback_radius_fraction = 3.0f;
    Vec3 zoomed_offset_radius_fractions{0, 0, 1.05f};
    float zoom_time = 0.65f;  // smoothstep 3t^2 - 2t^3 (RENDER.md §3.5)
    float right_button_scroll_factor = 0.25f;
    float scroll_plane = 10.0f;  // Galactic_Scroll_Plane: z of the camera's look-at point (P1_CAMERA_INPUT.md §1)
    Vec3 start_position{0, 0, 0};
    // Camera_Stop_Left / _Right are read only by dead code (no caller, STRATEGIC_GAPS §7.1) and Camera_Z_Position has no
    // reader: not parsed.
};
struct UiConstants {
    ScrollConstants tactical{2, 4000, 1000, 50, 2.0f};
    ScrollConstants strategic{2, 400, 80, 50, 2.0f};
    GalacticCameraConstants galactic;
    float minimum_drag_distance = 4.0f, minimum_drag_select_distance = 100.0f;
};
// Reads the fields above from the <GameConstants> root (missing ones keep their defaults).
UiConstants parse_ui_constants(const data::XmlNode& game_constants_root);

// Camera look-at limits. Land: the map's playable box; space: the 0x103 camera box (SPACE_AND_OPEN A.1); galactic: the
// union of the planets' boxes (galactic_scroll_box).
struct CameraBox {
    float min_x = -5000, min_y = -5000, max_x = 5000, max_y = 5000;
};

// Options and rates. Options (P1_CAMERA_INPUT.md §3.1; the host copies them from the profile / options menu):
struct CameraTuning {
    float scroll_speed_option = 1.0f;  // IDC_SCROLL_SPEED_SLIDER (0..3; at least 0.1 is used; Defaults = 1.0)
    bool windowed = false;             // windowed: edge speed by distance past the edge, keys = Max; fullscreen: midpoint
    bool absolute_scrolling = true;    // IDC_ABSOLUTE_SCROLLING: off -> a middle drag push-scrolls instead of panning
    bool right_button_scroll = false;  // IDC_RIGHT_BUTTON_SCROLL: a right drag > 0.08 drag units acts as a middle drag
    bool both_buttons_rotate = false;  // IDC_BOTH_MOUSE_BUTTONS_ROTATE: left + right held rotates (4 x the Ctrl rate)
    int32_t push_dead_zone_px = 10;    // 5 x Tactical_Edge_Scroll_Region (the tactical value in galactic too)
    // [port: Gabriel] keyboard camera axes (the original has none; a mouse-like feel would be ~1.5 deg per mouse unit).
    float key_yaw_deg_per_s = 90.0f;
    float key_zoom_notches_per_s = 4.0f;
    float overview_blend_time = 0.5f;   // overview level changes (smoothstep) [port]
    // [port: Gabriel] (docs/DECISIONS.md "Camera and controls"): the tactical camera rotates freely on every axis - yaw,
    // pitch and roll unclamped (wrapped to -180..180), pitch keys / drag also on Land_Mode (whose data locks the pitch;
    // free_pitch_per_mouse_unit then applies), roll keys. false = the original clamps (Yaw/Pitch_Min/Max).
    bool free_rotation = true;
    float key_pitch_deg_per_s = 60.0f, key_roll_deg_per_s = 60.0f;
    float free_pitch_per_mouse_unit = -1.5f;
};

// Terrain height under a world (x, y) (land maps; the host wraps terrain::Heightmap sampling).
using TerrainHeightFn = std::function<float(float x, float y)>;

// Galactic scroll box: union of centre +- half extent (= radius) over `items` (pass the planets).
CameraBox galactic_scroll_box(std::span<const Vec3> centres, std::span<const float> half_extents);

// Mean of `points` (summed in index order), nullopt when empty: the camera-jump target of a control group.
std::optional<Vec3> focus_point(std::span<const Vec3> points);

class TacticalCameraController {
public:
    explicit TacticalCameraController(TacticalCameraConstants c = space_mode_defaults(),
                                      ScrollConstants s = UiConstants{}.tactical, CameraTuning t = {});

    const TacticalCameraConstants& constants() const { return c_; }
    void set_bounds(const CameraBox& b);
    const CameraBox& bounds() const { return box_; }
    // Effective look-at limits: x in [min + Side, max - Side], y in [min + Bottom, max - Top] (a positive buffer shrinks).
    CameraBox limits() const;

    // Pitch / yaw / distance / FOV / spline fraction back to the XML defaults (snapped); the look-at point stays.
    void reset();
    void set_look_at(Vec3 p);  // clamped
    void set_yaw_deg(float y);
    void set_pitch_deg(float p);  // clamped; ignored while the land zoom spline drives pitch
    void set_distance(float d);   // clamped, immediate (no smoothing); also the matching spline fraction

    // One frame of user control: keys, edge scroll, mouse drags, wheel, then smoothing. dt = real seconds (capped at 1).
    void update(const InputState& in, const Bindings& keys, float dt);
    // Direct controls (update() uses these).
    void pan(float right, float forward);       // world units along the camera heading
    void rotate(float dyaw_deg, float dpitch_deg);
    void roll(float droll_deg);                 // [port: Gabriel] free rotation only
    void zoom(float notches);                   // + = in; moves the distance / FOV / spline-fraction targets
    void step(float dt);                        // smoothing towards the targets
    // Mouse drag mode of this frame (middle / right-scroll / both-buttons drag).
    enum class Drag : uint8_t { None, Pan, Rotate };
    Drag drag() const { return drag_; }

    // Overview zoom (see the header comment). Level 0 = normal, 1 / 2 = Tactical_Overview_* / *2.
    int32_t overview_level() const { return ov_level_; }
    int32_t max_overview_level() const;
    void set_overview(int32_t level);  // clamped to 0..max_overview_level()
    void toggle_overview();            // 0 -> 1 -> 2 -> 0 (hotkey slot 76)
    bool overview_transitioning() const { return ov_t_ < 1.0f; }
    // Terrain following: null = flat (look-at z as set). Setting a callback snaps the look-at height.
    void set_terrain_height(TerrainHeightFn fn);
    // Moves the look-at point (control-group camera jump, radar jump); yaw / pitch / zoom kept.
    void jump_to(Vec3 p);
    // [port: Gabriel] field of view setting (degrees, defined for 4:3 like the data; <= 0 = TacticalCameras.xml FOV).
    // Presentation only. Replaces the normal-zoom FOV; the overview levels keep their own.
    void set_fov_override(float degrees) { fov_override_ = degrees; }
    float fov_override() const { return fov_override_; }
    float roll_deg() const { return roll_; }
    const CameraTuning& tuning() const { return t_; }
    void set_tuning(const CameraTuning& t) { t_ = t; }

    Vec3 look_at() const { return look_; }
    float yaw_deg() const { return yaw_; }
    float pitch_deg() const;     // effective (spline when engaged, overview blend)
    float distance() const;      // effective
    float target_distance() const { return dist_.target; }
    float fov_deg() const;       // effective (overview blend)
    float zoom_fraction() const;  // 0 = fully in, 1 = fully out (the smoothed spline fraction with splines)
    float spline_fraction_target() const { return spl_.target; }
    bool splines_engaged() const { return c_.use_splines && ov_level_ == 0; }
    float scroll_speed() const;   // world units / s of a held scroll key at the current distance
    // Smoothed scroll velocity in speed units / s (x 3e-4 x distance = world units / s; y screen-down) and a hard stop.
    float scroll_velocity_x() const { return scroll_vx_; }
    float scroll_velocity_y() const { return scroll_vy_; }
    void stop_scroll() { scroll_vx_ = scroll_vy_ = 0.0f; }

    // Pose for the renderer / picking; pitch raised so the eye clears the terrain by Min_Height_Above_Terrain.
    CameraParams params(float aspect) const;

private:
    struct Pose {
        float distance, pitch, fov;
    };
    struct Smoothed {
        float value = 0, target = 0, vel = 0;
        void snap(float v) { value = target = v, vel = 0; }
    };
    Pose normal_pose() const;
    Pose level_pose(int32_t level) const;
    Pose effective_pose() const;
    void overview_click(float notches);
    void mouse(const InputState& in);

    TacticalCameraConstants c_;
    ScrollConstants s_;
    CameraTuning t_;
    CameraBox box_;
    Vec3 look_{};
    float yaw_ = 0, pitch_ = 50;
    Smoothed dist_, fov_, spl_, height_s_;
    Drag drag_ = Drag::None;
    bool drag_moved_ = false;  // the middle press moved: no reset on release
    float right_acc_ = 0.0f;   // right-button scroll: accumulated drag units
    bool right_active_ = false;
    // overview
    int32_t ov_level_ = 0;
    Pose ov_from_{1000, 50, 55};
    float ov_t_ = 1.0f;
    float ov_clicks_ = 0.0f;
    double ov_click_start_ = -1e30;
    double time_ = 0.0;
    // terrain
    TerrainHeightFn height_;
    // [port: Gabriel] free rotation / FOV setting
    float pitch_free_ = 0.0f, roll_ = 0.0f, fov_override_ = 0.0f;
    float scroll_vx_ = 0.0f, scroll_vy_ = 0.0f;  // scroll smoothing (STRATEGIC_GAPS §7.1)
};

// Galactic map camera (RENDER.md §3.5 [M], SPACE_AND_OPEN B.8): looks north (+Y) pitched down by
// -GMC_InitialPitchAngleDegrees, pulled back GMC_InitialPullbackDistance; zooming onto a planet blends
// (smoothstep over GMC_ZoomTime) to pitch -GMC_ZoomedPitch and pullback radius * fraction around
// planet + offset * radius.
class GalacticCameraController {
public:
    explicit GalacticCameraController(GalacticCameraConstants c = {}, ScrollConstants s = UiConstants{}.strategic,
                                      CameraTuning t = {});
    // Planet union box (galactic_scroll_box); the look-at is clamped to it widened by 15 % of each half extent.
    void set_bounds(const CameraBox& b);
    CameraBox limits() const;
    void set_look_at(Vec3 p);  // z := Galactic_Scroll_Plane
    void update(const InputState& in, const Bindings& keys, float dt);
    void pan(float dx, float dy);  // only while zoomed out and settled (zoom state 0)
    // Wheel: notches > 0 = zoom-in request for the host (take_zoom_in_request: the hovered or selected planet ->
    // zoom_to_planet), < 0 = zoom out from the settled zoomed view. No continuous zoom (P1_CAMERA_INPUT.md §5).
    void zoom(float notches);
    bool take_zoom_in_request() { return std::exchange(zoom_in_request_, false); }
    // Options (scroll slider, windowed, absolute / right-button scrolling); look-at, bounds, FOV and zoom state are kept.
    void set_tuning(const CameraTuning& t) { t_ = t; }
    const CameraTuning& tuning() const { return t_; }
    void zoom_to_planet(Vec3 planet, float radius);
    void zoom_out();
    void step(float dt);

    Vec3 look_at() const;
    float pitch_deg() const;   // down angle (positive)
    float pullback() const;
    bool zoomed() const { return zoomed_; }
    bool transitioning() const { return t_anim_ < 1.0f; }
    // Zoom transition progress z (0..1, linear in time over GMC_ZoomTime, before the pose smoothstep; 1 = settled).
    // Direction: zoomed() (true = zooming / zoomed in, false = zooming / zoomed out).
    float zoom_progress() const { return t_anim_; }
    // Route-colour blend f: 0 zoomed out, z while zooming in, 1 - z while zooming out, 1 zoomed in. A reversal mid-way
    // restarts z at 0 (the pose blends from where it was), so f jumps there.
    float zoom_blend() const { return zoomed_ ? t_anim_ : 1.0f - t_anim_; }
    // Zoom state (GalacticCameraClass +0x44): 0 free, 1 zooming in, 2 zooming out, 3 zoomed in.
    int32_t zoom_state() const { return t_anim_ < 1.0f ? (zoomed_ ? 1 : 2) : (zoomed_ ? 3 : 0); }
    float scroll_velocity_x() const { return scroll_vx_; }  // key / edge scroll smoothing (ScrollConstants), world u / s
    float scroll_velocity_y() const { return scroll_vy_; }
    void stop_scroll() { scroll_vx_ = scroll_vy_ = 0.0f; }
    // [port: Gabriel] horizontal field of view setting (degrees; <= 0 = atan(0.5), SPACE_AND_OPEN B.8). Presentation only.
    void set_fov_deg(float degrees) { fov_deg_ = degrees; }
    float fov_deg() const;
    CameraParams params(float aspect) const;

private:
    struct Pose {
        Vec3 look;
        float pitch, pullback;
    };
    Pose free_pose() const;
    Pose zoom_pose() const;
    Pose blended() const;
    GalacticCameraConstants c_;
    ScrollConstants s_;
    CameraTuning t_;
    CameraBox box_{-1e9f, -1e9f, 1e9f, 1e9f};
    Vec3 look_{};
    bool zoomed_ = false;
    bool zoom_in_request_ = false;
    bool dragging_ = false;
    Vec3 planet_{};
    float planet_radius_ = 0;
    Pose from_{};
    float t_anim_ = 1.0f;
    float fov_deg_ = 0.0f;
    float scroll_vx_ = 0.0f, scroll_vy_ = 0.0f;
};

}  // namespace alamo::ui
