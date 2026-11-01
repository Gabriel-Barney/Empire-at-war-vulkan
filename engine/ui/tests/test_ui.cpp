// alamo_ui tests (no framework; non-zero exit on failure): input state machine and bindings, camera math
// (pose, limits, clamping, bounds, Hor+ FOV, splines, galactic zoom), picking against a synthetic TreeSet,
// box select, click -> command translation with exact quantized payloads, input replay determinism.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/input.hpp"
#include "alamo/ui/orders.hpp"
#include "alamo/ui/picking.hpp"

using namespace alamo;
using namespace alamo::ui;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// Builds frames for scripted input.
struct Script {
    InputFrame f;
    std::vector<InputFrame> frames;
    Script() {
        f.width = 800;
        f.height = 600;
        f.dt = 1.0f / 60.0f;
        f.mouse_x = 400;
        f.mouse_y = 300;
    }
    InputFrame& next() {  // advance time, clear edges
        f.time += f.dt;
        f.keys_pressed = {};
        f.buttons_pressed = f.buttons_released = 0;
        f.wheel = 0;
        return f;
    }
    void push() { frames.push_back(f); }
    void idle() {
        next();
        push();
    }
    void move(int32_t x, int32_t y) {
        next();
        f.mouse_x = x;
        f.mouse_y = y;
        push();
    }
    void down(Button b) {
        next();
        f.buttons_down |= button_bit(b);
        f.buttons_pressed |= button_bit(b);
        push();
    }
    void up(Button b) {
        next();
        f.buttons_down = static_cast<uint8_t>(f.buttons_down & ~button_bit(b));
        f.buttons_released |= button_bit(b);
        push();
    }
    void click(Button b, int32_t x, int32_t y) {
        move(x, y);
        down(b);
        up(b);
    }
    void key(uint16_t k, bool d) {
        next();
        f.keys_down.set(k, d);
        if (d) f.keys_pressed.set(k);
        push();
    }
    void tap(uint16_t k) {
        key(k, true);
        key(k, false);
    }
    void wheel(float n) {
        next();
        f.wheel = n;
        push();
    }
};

// ---- input -------------------------------------------------------------------------------------------
void test_input() {
    InputState in;
    Script s;
    s.idle();
    in.update(s.frames.back());
    CHECK(!in.button(Button::Left).down);

    // click: press + release without motion
    s.down(Button::Left);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).pressed && in.button(Button::Left).down && !in.button(Button::Left).click);
    s.up(Button::Left);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).released && in.button(Button::Left).click && !in.button(Button::Left).double_click);
    // second click within 0.5 s and 4 px: double click
    s.move(402, 301);
    in.update(s.frames.back());
    s.down(Button::Left);
    in.update(s.frames.back());
    s.up(Button::Left);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).click && in.button(Button::Left).double_click);
    // third click: not a double click again
    s.down(Button::Left);
    in.update(s.frames.back());
    s.up(Button::Left);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).click && !in.button(Button::Left).double_click);
    // late second click: no double
    for (int i = 0; i < 40; ++i) {
        s.idle();
        in.update(s.frames.back());
    }
    s.down(Button::Left);
    in.update(s.frames.back());
    s.up(Button::Left);
    in.update(s.frames.back());
    CHECK(!in.button(Button::Left).double_click);

    // drag: 3 px is not a drag, 5 px is
    s.down(Button::Left);
    in.update(s.frames.back());
    s.move(405, 301);  // from 402,301: 3 px
    in.update(s.frames.back());
    CHECK(!in.button(Button::Left).dragging);
    s.move(407, 301);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).dragging && in.button(Button::Left).dx == 2);
    s.up(Button::Left);
    in.update(s.frames.back());
    CHECK(in.button(Button::Left).drag_ended && !in.button(Button::Left).click);
    CHECK(in.button(Button::Left).drag.x0 == 402 && in.button(Button::Left).drag.x1 == 407);

    // tap inside one frame (pressed + released edges, never down): still a click
    s.next();
    s.f.buttons_pressed = button_bit(Button::Right);
    s.f.buttons_released = button_bit(Button::Right);
    s.push();
    in.update(s.frames.back());
    CHECK(in.button(Button::Right).click && !in.button(Button::Right).down);

    // keys: edge from the pump flag and from the down change
    s.key(vk::kUp, true);
    in.update(s.frames.back());
    CHECK(in.key_down(vk::kUp) && in.key_pressed(vk::kUp));
    s.idle();
    in.update(s.frames.back());
    CHECK(in.key_down(vk::kUp) && !in.key_pressed(vk::kUp));
    s.key(vk::kUp, false);
    in.update(s.frames.back());
    CHECK(in.key_released(vk::kUp));

    // bindings: defaults, modifiers, rebinding
    Bindings b;
    s.key(vk::kControl, true);
    in.update(s.frames.back());
    s.key(vk::key('Y'), true);  // [port: Gabriel] Ctrl+Y (W A S D only move)
    in.update(s.frames.back());
    CHECK(b.pressed(in, Action::SelectAll));
    s.key(vk::key('Y'), false);
    s.key(vk::kControl, false);
    in.update(s.frames.back());
    s.key(vk::kA, true);
    in.update(s.frames.back());
    CHECK(!b.pressed(in, Action::SelectAll));  // needs Ctrl
    b.bind(Action::SelectAll, 1, {vk::kA, kModNone});
    CHECK(b.pressed(in, Action::SelectAll));
    b.clear(Action::SelectAll);
    CHECK(!b.pressed(in, Action::SelectAll));
    CHECK(action_name(Action::ControlGroup3) == "ControlGroup3");
}

// ---- camera ------------------------------------------------------------------------------------------
// SmoothCD reference (P1_CAMERA_INPUT.md §4.1), computed independently of camera.cpp.
float smooth_ref(float cur, float vel, float target, float st, float dt) {
    const double w = 2.0 / st, x = w * dt, e = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    const double c = static_cast<double>(cur) - target, tmp = (vel + w * c) * dt;
    return static_cast<float>(target + (c + tmp) * e);
}
template <class Cam>
void settle(Cam& c) {
    for (int i = 0; i < 400; ++i) c.step(0.05f);
}
// One update() with `f` as the current frame.
template <class Cam>
void frame(Cam& c, InputState& in, const Bindings& k, const InputFrame& f, float dt) {
    in.update(f);
    c.update(in, k, dt);
}

void test_camera() {
    // The original clamps (free_rotation off; [port: Gabriel] free rotation is the default, test_controls).
    CameraTuning clamped;
    clamped.free_rotation = false;
    // Space_Mode pose (SPACE_AND_OPEN D.7 2): yaw 0, pitch 50, distance 1000 -> eye 766 above, 643 behind.
    TacticalCameraController space(space_mode_defaults(), UiConstants{}.tactical, clamped);
    CameraParams p = space.params(4.0f / 3.0f);
    CHECK(near(p.eye.x, 0.0f) && near(p.eye.y, -642.788f, 0.01f) && near(p.eye.z, 766.044f, 0.01f));
    CHECK(near(p.fov_x, 55.0f * kDegToRad, 1e-6f));
    CHECK(p.znear == 10.0f && p.zfar == 7000.0f && p.fade_begin == 6000.0f && p.fade_end == 7000.0f);
    CHECK(near(p.orbit_pitch, 50.0f * kDegToRad, 1e-6f) && near(p.orbit_yaw, -0.5f * kPi, 1e-6f));
    // Hor+ above 4:3: 2 atan(tan(27.5 deg) * 0.75 * 16/9); 21:9 gets the 16:9 value
    const float hp = 2.0f * std::atan(std::tan(27.5f * kDegToRad) * 0.75f * 16.0f / 9.0f);
    CHECK(near(space.params(16.0f / 9.0f).fov_x, hp, 1e-6f));
    CHECK(near(space.params(21.0f / 9.0f).fov_x, hp, 1e-6f));
    // Distance: wheel moves the target (500 / notch, clamped 200..1900); SmoothCD (Distance_Smooth_Time 0.1) follows.
    space.zoom(100.0f);
    CHECK(space.target_distance() == 200.0f);
    space.step(0.05f);
    CHECK(near(space.distance(), smooth_ref(1000.0f, 0.0f, 200.0f, 0.1f, 0.05f), 0.01f));
    CHECK(space.distance() > 200.0f && space.distance() < 1000.0f);
    settle(space);
    CHECK(near(space.distance(), 200.0f, 0.01f));
    space.zoom(-100.0f);
    settle(space);
    CHECK(near(space.distance(), 1900.0f, 0.01f));
    {  // the spring is nearly frame-rate independent: 10 x 0.01 s ~ 1 x 0.1 s (the closed form is an approximation)
        TacticalCameraController a(space_mode_defaults()), b(space_mode_defaults());
        a.zoom(1.0f);
        b.zoom(1.0f);
        for (int i = 0; i < 10; ++i) a.step(0.01f);
        b.step(0.1f);
        CHECK(std::fabs(a.distance() - b.distance()) < 25.0f);
        float v = 0, x = 5;
        smooth_cd(x, v, 1, 0.0f, 0.1f);  // smooth time 0 snaps
        CHECK(x == 1.0f && v == 0.0f);
    }
    space.rotate(0.0f, 100.0f);
    CHECK(space.pitch_deg() == 85.0f);
    space.rotate(0.0f, -200.0f);
    CHECK(space.pitch_deg() == -10.0f);
    space.rotate(5000.0f, 0.0f);  // wrapped to (-180, 180], then clamped to +-1000
    CHECK(near(space.yaw_deg(), -40.0f));
    space.reset();
    CHECK(space.pitch_deg() == 50.0f && space.distance() == 1000.0f && space.yaw_deg() == 0.0f);
    // bounds: camera box +-4600 with the -300 buffers -> a negative buffer enlarges: look-at within +-4900
    space.set_bounds({-4600, -4600, 4600, 4600});
    space.set_look_at({10000, -10000, 0});
    CHECK(space.look_at().x == 4900.0f && space.look_at().y == -4900.0f);
    {  // a positive buffer shrinks
        TacticalCameraConstants k = space_mode_defaults();
        k.side_bounds_buffer = 100, k.top_bounds_buffer = 50, k.bottom_bounds_buffer = 20;
        TacticalCameraController b(k);
        b.set_bounds({-1000, -1000, 1000, 1000});
        const CameraBox l = b.limits();
        CHECK(l.min_x == -900.0f && l.max_x == 900.0f && l.min_y == -980.0f && l.max_y == 950.0f);
    }
    // pan follows yaw: yaw 90 -> forward = +X, right = -Y
    space.set_look_at({0, 0, 0});
    space.set_yaw_deg(90.0f);
    space.pan(0.0f, 100.0f);
    CHECK(near(space.look_at().x, 100.0f) && near(space.look_at().y, 0.0f));
    space.pan(50.0f, 0.0f);
    CHECK(near(space.look_at().x, 100.0f) && near(space.look_at().y, -50.0f));

    // Land_Mode: the splines are in force from the first frame (fraction 1.0): pitch 55 / distance 460, not Pitch_Default 50.
    TacticalCameraController land(land_mode_defaults(), UiConstants{}.tactical, clamped);
    CHECK(land.splines_engaged() && land.zoom_fraction() == 1.0f);
    CHECK(land.pitch_deg() == 55.0f && land.distance() == 460.0f && land.fov_deg() == 55.0f);
    land.rotate(0.0f, 20.0f);
    CHECK(land.pitch_deg() == 55.0f);
    land.zoom(1.0f);  // spline fraction target -1/15, distance target -100
    CHECK(near(land.spline_fraction_target(), 14.0f / 15.0f, 1e-6f) && land.target_distance() == 360.0f);
    settle(land);
    const float t = 14.0f / 15.0f;
    CHECK(near(land.distance(), 220.0f + (460.0f - 220.0f) * (t - 0.3f) / 0.7f, 1e-2f));
    CHECK(near(land.pitch_deg(), 45.0f + 10.0f * (t - 0.35f) / 0.65f, 1e-2f));
    land.zoom(20.0f);
    settle(land);
    CHECK(near(land.distance(), 170.0f, 1e-2f) && near(land.pitch_deg(), 27.0f, 1e-2f));
    CHECK(land.params(1.5f).zfar == 7000.0f && land.params(1.5f).fade_end == 1300.0f);
    land.reset();  // the spline fraction snaps back to 1
    CHECK(land.zoom_fraction() == 1.0f && land.pitch_deg() == 55.0f);
    // land buffers 0: limits = box
    land.set_bounds({-1000, -500, 1000, 500});
    CHECK(land.limits().max_x == 1000.0f && land.limits().min_y == -500.0f);
    {  // no splines: Pitch_Zoom_Begin_Fraction lowers the used pitch towards Pitch_When_Zoomed_In below it
        TacticalCameraConstants k = space_mode_defaults();
        k.pitch_zoom_begin_fraction = 0.5f;
        k.pitch_when_zoomed_in = 20.0f;
        TacticalCameraController z(k, UiConstants{}.tactical, clamped);
        z.set_distance(200.0f);  // fraction 0 -> fully the zoomed-in pitch
        CHECK(near(z.pitch_deg(), 20.0f));
        z.set_distance(200.0f + 0.25f * 1700.0f);  // fraction 0.25: halfway
        CHECK(near(z.pitch_deg(), 35.0f, 1e-3f));
    }

    // Keyboard / edge scrolling (§2): fullscreen speed = Min/Max midpoint 2500, world speed = S x 3e-4 x distance.
    {
        Bindings keys;
        auto run = [&](const CameraTuning& tu, const InputFrame& f, float dt) {
            TacticalCameraController c(space_mode_defaults(), UiConstants{}.tactical, tu);
            InputState in;
            frame(c, in, keys, f, dt);
            return c.look_at();
        };
        Script s;
        const CameraTuning full;
        CameraTuning win;
        win.windowed = true;
        s.key(vk::key('D'), true);  // 0.5 s = 15 updates: 15 x 0.08 > 1, full speed at once
        CHECK(near(run(full, s.f, 0.5f).x, 2500.0f * 3e-4f * 1000.0f * 0.5f, 0.05f));
        CHECK(near(run(win, s.f, 0.5f).x, 4000.0f * 0.3f * 0.5f, 0.05f));  // windowed keys: Max
        CameraTuning fast = full;
        fast.scroll_speed_option = 2.0f;
        CHECK(near(run(fast, s.f, 0.5f).x, 750.0f, 0.05f));
        fast.scroll_speed_option = 0.0f;  // at least 0.1
        CHECK(near(run(fast, s.f, 0.5f).x, 37.5f, 0.01f));
        s.key(vk::key('D'), false);
        s.key(vk::key('W'), true);  // vertical x 1.2, forward
        const Vec3 fw = run(full, s.f, 0.5f);
        CHECK(near(fw.y, 450.0f, 0.05f) && near(fw.x, 0.0f));
        s.key(vk::key('W'), false);
        s.move(0, 300);  // 2 px past the 2 px edge region start: fullscreen midpoint, windowed Min + 2/50 (Max - Min)
        CHECK(near(run(full, s.f, 0.5f).x, -375.0f, 0.05f));
        CHECK(near(run(win, s.f, 0.5f).x, -(1000.0f + 3000.0f * 2.0f / 50.0f) * 0.15f, 0.05f));
        s.move(-10, 300);  // captured cursor 12 px past
        CHECK(near(run(win, s.f, 0.5f).x, -(1000.0f + 3000.0f * 12.0f / 50.0f) * 0.15f, 0.05f));
        s.move(-100, 300);  // clamped at the 50 px offscreen region: Max
        CHECK(near(run(win, s.f, 0.5f).x, -4000.0f * 0.15f, 0.05f));
        s.move(799, 599);  // bottom-right corner: right and back
        const Vec3 br = run(full, s.f, 0.5f);
        CHECK(near(br.x, 375.0f, 0.05f) && near(br.y, -450.0f, 0.05f));
        s.move(0, 300);
        s.f.mouse_inside = false;  // cursor outside the window, no drag: no edge scroll
        CHECK(run(full, s.f, 0.5f).x == 0.0f);
        s.f.mouse_inside = true;
        s.f.buttons_down = button_bit(Button::Left);  // drag-select pending: no edge scroll
        CHECK(run(full, s.f, 0.5f).x == 0.0f);
        s.f.buttons_down = 0;
        CHECK(near(TacticalCameraController().scroll_speed(), 750.0f, 0.01f));
    }
    // Scroll smoothing (§2.2), per-second form at 30 updates / s: +0.08 x T per update up to T, then / 1.5 per update,
    // 0 once below 0.1 speed units per update (3 / s).
    {
        ScrollConstants sc = UiConstants{}.tactical;
        const float u = 1.0f / 30.0f;
        float vx = 0, vy = 0;
        scroll_smooth(2500.0f, 0.0f, u, sc, vx, vy);
        CHECK(near(vx, 200.0f, 1e-3f) && vy == 0.0f);
        for (int i = 0; i < 20; ++i) scroll_smooth(2500.0f, 0.0f, u, sc, vx, vy);
        CHECK(vx == 2500.0f);  // clamped at T
        scroll_smooth(-2500.0f, 0.0f, u, sc, vx, vy);
        CHECK(near(vx, 2300.0f, 1e-2f));  // reversing decelerates through the accelerating step
        vx = 2500.0f;
        int steps = 0;
        while (vx != 0.0f && steps < 100) {
            scroll_smooth(0.0f, 0.0f, u, sc, vx, vy);
            ++steps;
        }
        CHECK(steps == 17);  // 2500 / 1.5^17 = 2.5 < 3
        vx = 500.0f;
        scroll_smooth(0.0f, 1000.0f, u, sc, vx, vy);  // a zero component of a non-zero T is cut at once
        CHECK(vx == 0.0f && near(vy, 80.0f, 1e-3f));
        // The tactical camera drifts after the key is released, then stops.
        TacticalCameraController c;
        Bindings keys;
        InputState in;
        Script s;
        s.key(vk::key('D'), true);
        for (int i = 0; i < 30; ++i) frame(c, in, keys, s.f, u);
        CHECK(c.scroll_velocity_x() == 2500.0f);
        s.key(vk::key('D'), false);
        const float x0 = c.look_at().x;
        frame(c, in, keys, s.f, u);
        CHECK(c.look_at().x > x0 && near(c.scroll_velocity_x(), 2500.0f / 1.5f, 1e-2f));
        for (int i = 0; i < 20; ++i) c.update(in, keys, u);
        const float x1 = c.look_at().x;
        c.update(in, keys, u);
        CHECK(c.scroll_velocity_x() == 0.0f && c.look_at().x == x1);
    }
    // Mouse (§3): middle drag pans 4 x distance / width per pixel with the mouse; Ctrl + middle rotates 150 deg / width;
    // a middle click without movement resets; the wheel is ignored during a drag.
    {
        TacticalCameraController c;
        Bindings keys;
        InputState in;
        Script s;
        s.move(400, 300);
        s.down(Button::Middle);
        frame(c, in, keys, s.f, 0.0f);
        s.move(410, 294);  // 10 px right, 6 px up
        frame(c, in, keys, s.f, 0.0f);
        CHECK(c.drag() == TacticalCameraController::Drag::Pan);
        CHECK(near(c.look_at().x, 4.0f * 1000.0f * 10.0f / 800.0f, 1e-3f) && near(c.look_at().y, 4.0f * 1000.0f * 6.0f / 600.0f, 1e-3f));
        s.wheel(1.0f);
        frame(c, in, keys, s.f, 0.0f);
        CHECK(c.target_distance() == 1000.0f);  // ignored while dragging
        s.key(vk::kControl, true);  // Ctrl during the drag switches to rotate
        frame(c, in, keys, s.f, 0.0f);
        s.move(420, 288);
        frame(c, in, keys, s.f, 0.0f);
        CHECK(c.drag() == TacticalCameraController::Drag::Rotate);
        CHECK(near(c.yaw_deg(), 150.0f * 10.0f / 800.0f, 1e-4f));
        CHECK(near(c.pitch_deg(), 50.0f - 1.5f * 100.0f * 6.0f / 600.0f, 1e-4f));  // mouse up lowers the pitch
        s.key(vk::kControl, false);
        s.up(Button::Middle);
        frame(c, in, keys, s.f, 0.0f);
        CHECK(c.drag() == TacticalCameraController::Drag::None && near(c.yaw_deg(), 1.875f, 1e-4f));  // no reset after a drag
        s.down(Button::Middle);
        frame(c, in, keys, s.f, 0.0f);
        s.up(Button::Middle);
        frame(c, in, keys, s.f, 0.0f);
        CHECK(c.yaw_deg() == 0.0f && c.pitch_deg() == 50.0f);  // middle click: reset
        s.wheel(1.0f);  // wheel zooms (one notch = 500 units)
        frame(c, in, keys, s.f, 1.0f);
        CHECK(c.target_distance() == 500.0f);
        settle(c);
        CHECK(near(c.distance(), 500.0f, 0.01f));
        // both-buttons rotate (option): 600 deg / width
        CameraTuning both;
        both.both_buttons_rotate = true;
        TacticalCameraController r(space_mode_defaults(), UiConstants{}.tactical, both);
        InputState in2;
        Script s2;
        s2.down(Button::Left);
        s2.down(Button::Right);
        for (const InputFrame& f : s2.frames) frame(r, in2, keys, f, 0.0f);
        s2.move(408, 300);
        frame(r, in2, keys, s2.f, 0.0f);
        CHECK(near(r.yaw_deg(), 600.0f * 8.0f / 800.0f, 1e-4f));
        // absolute scrolling off: a middle drag push-scrolls from the press point past a 10 px dead zone (x 2)
        CameraTuning push;
        push.absolute_scrolling = false;
        TacticalCameraController q(space_mode_defaults(), UiConstants{}.tactical, push);
        InputState in3;
        Script s3;
        s3.down(Button::Middle);
        frame(q, in3, keys, s3.f, 0.0f);
        s3.move(400 + 10 + 25, 300);
        frame(q, in3, keys, s3.f, 0.5f);
        CHECK(near(q.look_at().x, 2500.0f * 2.0f * 0.3f * 0.5f, 0.05f));
    }

    // Galactic (B.8 / RENDER 3.5): FOV atan(0.5), near 10 / far 10000, pitch 30 down, pullback 700 behind the look-at
    // point on Galactic_Scroll_Plane (z 10).
    GalacticCameraController g;
    CameraParams gp = g.params(16.0f / 9.0f);
    CHECK(g.zoom_progress() == 1.0f && g.zoom_blend() == 0.0f && g.zoom_state() == 0);  // settled, zoomed out
    CHECK(gp.galactic && gp.fov_x == kGalacticFovX && gp.znear == 10.0f && gp.zfar == 10000.0f);
    CHECK(g.look_at().z == 10.0f && near(gp.eye.y, -700.0f * std::cos(30.0f * kDegToRad), 0.01f) && near(gp.eye.z, 360.0f, 0.01f));
    g.set_look_at({5, 6, 0});
    CHECK(g.look_at().z == 10.0f);  // set position: z := plane
    g.set_look_at({0, 0, 0});
    g.zoom_to_planet({100, 200, 0}, 20.0f);
    CHECK(g.zoom_progress() == 0.0f && g.zoom_blend() == 0.0f && g.zoomed() && g.zoom_state() == 1);
    g.step(0.325f);  // half of GMC_ZoomTime: smoothstep(0.5) = 0.5
    CHECK(near(g.zoom_progress(), 0.5f, 1e-5f) && near(g.zoom_blend(), 0.5f, 1e-5f));  // zooming in: f = z
    CHECK(near(g.pullback(), 0.5f * (700.0f + 60.0f), 0.01f) && near(g.pitch_deg(), 32.5f, 1e-3f));
    g.pan(100, 0);  // not while zooming
    g.step(1.0f);
    CHECK(!g.transitioning() && near(g.look_at().z, 21.0f) && g.pullback() == 60.0f && g.pitch_deg() == 35.0f);
    CHECK(g.zoom_progress() == 1.0f && g.zoom_blend() == 1.0f && g.zoom_state() == 3);  // zoomed in
    g.zoom(-1.0f);  // wheel down from the zoomed view: zoom out
    CHECK(g.zoom_progress() == 0.0f && g.zoom_blend() == 1.0f && !g.zoomed() && g.zoom_state() == 2);
    g.step(0.1625f);  // a quarter: zooming out, f = 1 - z
    CHECK(near(g.zoom_progress(), 0.25f, 1e-5f) && near(g.zoom_blend(), 0.75f, 1e-5f));
    g.step(1.0f);
    CHECK(g.zoom_progress() == 1.0f && g.zoom_blend() == 0.0f);
    CHECK(g.pullback() == 700.0f && g.look_at().x == 0.0f && g.look_at().z == 10.0f);
    // wheel up: a zoom-in request for the host (no continuous zoom)
    CHECK(!g.take_zoom_in_request());
    g.zoom(1.0f);
    CHECK(g.pullback() == 700.0f && g.take_zoom_in_request() && !g.take_zoom_in_request());
    g.zoom(-1.0f);  // nothing to zoom out of
    CHECK(g.zoom_state() == 0);
    // clamp: planet box widened by 15 % of its half extents
    g.set_bounds({-50, -50, 50, 50});
    g.pan(1000, -1000);
    CHECK(near(g.look_at().x, 57.5f) && near(g.look_at().y, -57.5f));
    const Vec3 cs[2] = {{0, 0, 0}, {100, 40, 0}};
    const float hs[2] = {10, 20};
    const CameraBox gb = galactic_scroll_box(cs, hs);
    CHECK(gb.min_x == -10.0f && gb.min_y == -10.0f && gb.max_x == 120.0f && gb.max_y == 60.0f);
    {  // keys: S world units / s (240 fullscreen, x 1.2 vertical); middle drag 1000 x px / width; right drag needs the option
        Bindings keys;
        GalacticCameraController k;
        InputState in;
        Script s;
        s.key(vk::key('D'), true);
        frame(k, in, keys, s.f, 0.5f);
        CHECK(near(k.look_at().x, 120.0f, 0.01f));
        s.key(vk::key('D'), false);
        s.key(vk::key('W'), true);
        GalacticCameraController k2;
        frame(k2, in, keys, s.f, 0.5f);
        CHECK(near(k2.look_at().y, 144.0f, 0.01f) && k2.look_at().x == 0.0f);
        s.key(vk::key('W'), false);
        GalacticCameraController m;
        s.down(Button::Middle);
        frame(m, in, keys, s.f, 0.0f);
        s.move(440, 330);  // 40 px right, 30 px down
        frame(m, in, keys, s.f, 0.0f);
        CHECK(near(m.look_at().x, 1000.0f * 40.0f / 800.0f, 1e-3f) && near(m.look_at().y, -1000.0f * 30.0f / 600.0f, 1e-3f));
        s.up(Button::Middle);
        frame(m, in, keys, s.f, 0.0f);
        GalacticCameraController r;
        s.down(Button::Right);
        frame(r, in, keys, s.f, 0.0f);
        s.move(480, 330);
        frame(r, in, keys, s.f, 0.0f);
        CHECK(r.look_at().x == 0.0f);  // IDC_RIGHT_BUTTON_SCROLL off (default)
        s.up(Button::Right);
        frame(r, in, keys, s.f, 0.0f);
        // wheel up -> request; ignored while dragging
        s.wheel(1.0f);
        frame(r, in, keys, s.f, 0.0f);
        CHECK(r.take_zoom_in_request());
    }
}

void test_parse() {
    const char* cams = R"(<TacticalCameras>
  <TacticalCamera Name="Land_Mode">
    <Pitch_Default>50</Pitch_Default><Pitch_Min>50</Pitch_Min><Pitch_Max>50</Pitch_Max>
    <Distance_Default>460.0</Distance_Default><Distance_Max>460.0</Distance_Max><Distance_Min>170.0</Distance_Min>
    <Use_Splines>yes</Use_Splines>
    <Pitch_Spline>   0.0,27.0, 0.35,45.0, 1.0, 55.0, 1.1,55 </Pitch_Spline>
    <Distance_Spline>0.0 170.0, 0.3, 220.0, 1.0, 460.0 </Distance_Spline>
    <Object_Fade_Begin>1100.0f</Object_Fade_Begin>
    <Location_Follows_Terrain>1</Location_Follows_Terrain>
  </TacticalCamera>
  <TacticalCamera Name="Space_Mode"><Pitch_Min>-10.0</Pitch_Min><Fov_Smooth_time>0.2</Fov_Smooth_time></TacticalCamera>
</TacticalCameras>)";
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(cams), "cams.xml");
    std::vector<TacticalCameraConstants> v;
    CHECK(parse_tactical_cameras(doc.root(), v) == 2);
    const TacticalCameraConstants* l = find_camera(v, "land_mode");
    CHECK(l != nullptr);
    if (l) {
        CHECK(l->use_splines && l->pitch_spline.x.size() == 4 && l->distance_spline.y.size() == 3);
        CHECK(l->distance_spline.y[1] == 220.0f && l->pitch_spline.x[3] == 1.1f && l->fade_begin == 1100.0f);
        CHECK(l->location_follows_terrain && l->distance_min == 170.0f);
    }
    const TacticalCameraConstants* sp = find_camera(v, "Space_Mode");
    CHECK(sp && sp->pitch_min == -10.0f && sp->fov_smooth_time == 0.2f);

    const char* gc = R"(<GameConstants>
  <Tactical_Edge_Scroll_Region>3</Tactical_Edge_Scroll_Region>
  <Strategic_Max_Scroll_Speed>400</Strategic_Max_Scroll_Speed> <!-- 800 -->
  <GMC_InitialPullbackDistance>650.0</GMC_InitialPullbackDistance>
  <GMC_ZoomedPositionOffsetPlanetRadiusFractions>0.0, 0.0, 1.5</GMC_ZoomedPositionOffsetPlanetRadiusFractions>
  <MinimumDragDistance> 6.0 </MinimumDragDistance>
  <Scroll_Acceleration_Factor>0.2</Scroll_Acceleration_Factor>
  <Scroll_Deceleration_Factor>2.0</Scroll_Deceleration_Factor>
  <Camera_Stop_Left>-50.0</Camera_Stop_Left>
</GameConstants>)";
    const data::XmlDocument gdoc = data::XmlDocument::parse(std::string_view(gc), "gc.xml");
    const UiConstants u = parse_ui_constants(gdoc.root());
    CHECK(u.tactical.edge_region_px == 3 && u.strategic.max_speed == 400.0f && u.galactic.initial_pullback == 650.0f);
    CHECK(u.galactic.zoomed_offset_radius_fractions.z == 1.5f && u.minimum_drag_distance == 6.0f);
    CHECK(u.tactical.max_speed == 4000.0f);  // default kept
    // Scroll smoothing factors reach both scroll sets (STRATEGIC_GAPS §7.1); Camera_Stop_* is dead code, not read.
    CHECK(u.tactical.acceleration == 0.2f && u.strategic.acceleration == 0.2f && u.tactical.deceleration == 2.0f &&
          u.strategic.deceleration == 2.0f);
}

// ---- picking -----------------------------------------------------------------------------------------
tactical::Aabb cube(float x, float y, float z, float r) { return {{x - r, y - r, z - r}, {x + r, y + r, z + r}}; }

void test_picking() {
    TacticalCameraController cam;  // space defaults, look-at origin
    const CameraParams p = cam.params(800.0f / 600.0f);
    const View v = make_view(p, 800, 600);
    // projection <-> ray round trip; the look-at point is the screen centre
    float sx = 0, sy = 0;
    CHECK(project(v, {0, 0, 0}, sx, sy) && near(sx, 400.0f, 0.01f) && near(sy, 300.0f, 0.01f));
    const tactical::Ray r = screen_ray(v, 400.0f, 300.0f);
    Vec3 g;
    CHECK(ray_plane_z(r, 0.0f, g) && near(g.x, 0.0f, 0.01f) && near(g.y, 0.0f, 0.01f));
    CHECK(project(v, {123, 456, 0}, sx, sy));
    CHECK(ray_plane_z(screen_ray(v, sx, sy), 0.0f, g) && near(g.x, 123.0f, 0.05f) && near(g.y, 456.0f, 0.05f));
    // a point behind the camera does not project
    CHECK(!project(v, {0, -2000, 1600}, sx, sy));

    tactical::TreeSet trees(2);
    trees.add(5, cube(0, 0, 0, 10), 1, {0});
    trees.add(3, cube(0, 0, 0, 10), 1, {0});    // same box: tie on t -> lower id first
    trees.add(9, cube(0, -100, 119.0f, 10), 1, {1});  // on the ray between the eye and the origin -> nearer
    trees.add(12, cube(300, 0, 0, 10), 1, {1});
    trees.add(20, cube(-300, 0, 0, 10), 1, {0});
    tactical::QueryContext ctx;
    auto hits = pick_all(trees.all(), r, tactical::kSkipDead, ctx);
    // the centre ray passes the origin; object 9 sits on the line of sight above it
    CHECK(project(v, {0, -100, 119.0f}, sx, sy) && near(sx, 400.0f, 0.5f));
    CHECK(hits.size() == 3);
    if (hits.size() == 3) {
        CHECK(hits[0].handle == 9 && hits[1].handle == 3 && hits[2].handle == 5);
        CHECK(hits[1].t == hits[2].t && hits[0].t < hits[1].t);
    }
    auto first = pick(trees.all(), r, tactical::kSkipDead, ctx);
    CHECK(first && first->id == 9);
    first = pick(trees.all(), r, tactical::kSkipDead, ctx, 9);
    CHECK(first && first->id == 3);
    trees.set_dead(9);
    first = pick(trees.all(), r, tactical::kSkipDead, ctx);
    CHECK(first && first->id == 3);
    trees.set_dead(9, false);
    // miss
    CHECK(!pick(trees.all(), screen_ray(v, 5.0f, 5.0f), tactical::kSkipDead, ctx));

    // refixf t08 world pick (HUD_GAPS §2.2) with host tests: precise per candidate, then the override sphere; nearest hit
    // by (t, resolved id); skip / resolve; NotModelled -> the tree box.
    {
        WorldPickTests wt;
        wt.precise = [r](uint32_t id, const tactical::Ray& ray, float& t) {
            const bool centre = ray.dir.x == r.dir.x && ray.dir.y == r.dir.y && ray.dir.z == r.dir.z;
            if (centre && (id == 3 || id == 5)) {  // the two origin cubes, centre ray: a precise hit at the same t
                t = 0.4f;
                return PreciseOutcome::Hit;
            }
            if (id == 20) return PreciseOutcome::NotModelled;
            return PreciseOutcome::Miss;  // 9 (nearer box) and 12: no mesh on the ray
        };
        auto first_w = pick_world(trees.all(), v, r, tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 3 && first_w->t == 0.4f);  // 9's box is nearer but its mesh misses; tie -> 3
        wt.skip = [](uint32_t id) { return id == 3; };
        first_w = pick_world(trees.all(), v, r, tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 5);
        wt.resolve = [](uint32_t id) { return id == 5 ? 77u : id; };
        first_w = pick_world(trees.all(), v, r, tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 77);
        wt.skip = nullptr;
        wt.resolve = [](uint32_t id) { return id == 3 ? 90u : id; };  // tie between 90 (from 3) and 5 -> 5
        first_w = pick_world(trees.all(), v, r, tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 5);
        wt.resolve = nullptr;
        // Sphere override on 12 (precise misses): a sphere at its centre around the ray's point.
        float sxx = 0, syy = 0;
        CHECK(project(v, {340, 0, 0}, sxx, syy));
        const tactical::Ray r12 = screen_ray(v, sxx, syy);  // through (340, 0, 0): off 12's 10-unit box
        CHECK(!pick(trees.all(), r12, tactical::kSkipDead, ctx));
        CHECK(!pick_world(trees.all(), v, r12, tactical::kSkipDead, ctx, &wt));
        wt.sphere = [](uint32_t id, Vec3& c) {
            if (id != 12) return 0.0f;
            c = {300, 0, 0};
            return 150.0f;
        };
        first_w = pick_world(trees.all(), v, r12, tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 12);
        // NotModelled: object 20's tree box.
        float s20x = 0, s20y = 0;
        CHECK(project(v, {-300, 0, 0}, s20x, s20y));
        first_w = pick_world(trees.all(), v, screen_ray(v, s20x, s20y), tactical::kSkipDead, ctx, &wt);
        CHECK(first_w && first_w->id == 20);
        // ray_sphere: entry t, origin inside -> 0, behind / beyond max_t -> miss.
        float ts = -1.0f;
        CHECK(ray_sphere({{0, -100, 0}, {0, 200, 0}, 1.0f}, {0, 0, 0}, 10.0f, ts) && near(ts, 0.45f, 1e-6f));
        CHECK(ray_sphere({{0, 5, 0}, {0, 200, 0}, 1.0f}, {0, 0, 0}, 10.0f, ts) && ts == 0.0f);
        CHECK(!ray_sphere({{0, 100, 0}, {0, 200, 0}, 1.0f}, {0, 0, 0}, 10.0f, ts));
        CHECK(!ray_sphere({{0, -100, 0}, {0, 200, 0}, 0.4f}, {0, 0, 0}, 10.0f, ts));
        CHECK(!ray_sphere({{20, -100, 0}, {0, 200, 0}, 1.0f}, {0, 0, 0}, 10.0f, ts));
    }

    // box select: rectangle around the centre takes 3, 5, 9 but not +-300 ones
    float ax = 0, ay = 0, bx = 0, by = 0;
    CHECK(project(v, {300, 0, 0}, ax, ay) && project(v, {-300, 0, 0}, bx, by));
    const DragRect centre{380, 320, 420, 270};  // any corner order
    auto sel = box_select(trees.all(), v, centre, tactical::kSkipDead, ctx);
    CHECK((sel == std::vector<uint32_t>{3, 5, 9}));
    const DragRect wide{0, 0, 799, 599};
    sel = box_select(trees.player(0), v, wide, tactical::kSkipDead, ctx);
    CHECK((sel == std::vector<uint32_t>{3, 5, 20}));
    const DragRect right_half{static_cast<int32_t>(ax) - 5, static_cast<int32_t>(ay) - 5, static_cast<int32_t>(ax) + 5,
                              static_cast<int32_t>(ay) + 5};
    sel = box_select(trees.all(), v, right_half, tactical::kSkipDead, ctx);
    CHECK((sel == std::vector<uint32_t>{12}));

    // galactic spheres: nearest entry by (t, id)
    const GalacticCameraController gcam;
    const View gv = make_view(gcam.params(800.0f / 600.0f), 800, 600);
    const PickSphere items[] = {{7, {0, 0, 0}, 10, -1, PickKind::Planet},
                                {4, {0, 0, 0}, 10, 0, PickKind::Fleet},
                                {2, {500, 0, 0}, 10, 0, PickKind::Fleet}};
    float t = 0;
    const auto gi = pick_spheres(items, screen_ray(gv, 400.0f, 300.0f), &t);
    CHECK(gi && items[*gi].id == 4 && t > 0.0f);
    CHECK(!pick_spheres(items, screen_ray(gv, 2.0f, 2.0f)));
}

// ---- orders ------------------------------------------------------------------------------------------
uint32_t qcode(float v) {  // COMMANDS.md §3.2 entry 1..3, computed independently
    const double step = 20000.0 / static_cast<double>((1u << 18) - 2u);
    return static_cast<uint32_t>((static_cast<double>(v) + 10000.0) / step + 0.5);
}

struct World {
    tactical::TreeSet trees{2};
    TacticalCameraController cam;
    World() {
        trees.add(3, cube(0, 0, 0, 10), 1, {0});
        trees.add(5, cube(60, 0, 0, 10), 1, {0});
        trees.add(9, cube(-300, 200, 0, 15), 1, {1});
        trees.add(11, cube(150, -150, 0, 10), 1, {0});
    }
    OrderContext ctx() const {
        OrderContext c;
        c.mode = UiMode::Space;
        c.local_player = 0;
        c.view = make_view(cam.params(800.0f / 600.0f), 800, 600);
        c.trees = &trees;
        return c;
    }
    void screen(Vec3 p, int32_t& x, int32_t& y) const {
        float sx = 0, sy = 0;
        project(ctx().view, p, sx, sy);
        x = static_cast<int32_t>(sx);
        y = static_cast<int32_t>(sy);
    }
};

void feed(InputState& in, OrderTranslator& ot, const Bindings& b, const OrderContext& c, Script& s, size_t from) {
    for (size_t i = from; i < s.frames.size(); ++i) {
        in.update(s.frames[i]);
        ot.update(in, b, c);
    }
}

void test_orders() {
    World w;
    const OrderContext c = w.ctx();
    Bindings keys;
    InputState in;
    OrderTranslator ot;
    Script s;
    int32_t x = 0, y = 0;

    // left click on own unit 5 -> Select{5}
    w.screen({60, 0, 0}, x, y);
    size_t mark = s.frames.size();
    s.click(Button::Left, x, y);
    feed(in, ot, keys, c, s, mark);
    auto out = ot.take();
    CHECK(out.size() == 1 && out[0].type() == commands::EventType::Select);
    if (out.size() == 1) CHECK((out[0].ids("ids") == std::vector<uint32_t>{5}));
    // shift click on 3 adds; ids sorted
    s.key(vk::kShift, true);
    w.screen({0, 0, 0}, x, y);
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();  // past the double-click window
    s.click(Button::Left, x, y);
    feed(in, ot, keys, c, s, mark - 1);
    out = ot.take();
    CHECK(out.size() == 1 && (out[0].ids("ids") == std::vector<uint32_t>{3, 5}));
    CHECK((ot.selection() == std::vector<uint32_t>{3, 5}));
    s.key(vk::kShift, false);
    in.update(s.frames.back());

    // right click on ground (space) -> MoveToRay{camera ray origin, point on z = 0, flags 0x04}
    mark = s.frames.size();
    s.click(Button::Right, 650, 150);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && out[0].type() == commands::EventType::MoveToRay);
    Vec3 gp;
    CHECK(ray_plane_z(pixel_ray(c.view, 650, 150), 0.0f, gp));
    if (out.size() == 1) {
        const Vec3 o = pixel_ray(c.view, 650, 150).origin;
        CHECK(out[0].u("bx") == qcode(gp.x) && out[0].u("by") == qcode(gp.y) && out[0].u("bz") == qcode(0.0f));
        CHECK(out[0].u("bz") == 131071u);  // 0 sits exactly on code (2^18 - 2) / 2
        CHECK(out[0].u("ax") == qcode(o.x) && out[0].u("ay") == qcode(o.y) && out[0].u("az") == qcode(o.z));
        CHECK(out[0].u("flags") == 0x04);
        CHECK(std::fabs(out[0].f("bx") - gp.x) <= 0.5f * commands::quantizer_step(1));
    }
    // land: right click on ground -> MoveToPosition with the exact quantized ground point
    {
        OrderContext cl = c;
        cl.mode = UiMode::Land;
        mark = s.frames.size();
        for (int i = 0; i < 40; ++i) s.idle();
        s.click(Button::Right, 650, 150);
        feed(in, ot, keys, cl, s, mark);
        out = ot.take();
        CHECK(out.size() == 1 && out[0].type() == commands::EventType::MoveToPosition);
        if (out.size() == 1) {
            CHECK(out[0].u("x") == qcode(gp.x) && out[0].u("y") == qcode(gp.y) && out[0].u("z") == qcode(0.0f));
            CHECK(out[0].u("flags") == 0x04 && out[0].u("cancel_activity") == 0);
            // canonical payload bytes: 5 fields x u32 (x, y, z, flags, cancel)
            std::vector<uint8_t> bytes;
            commands::encode_payload(out[0], bytes);
            CHECK(bytes.size() == 20);
            if (bytes.size() == 20) {
                uint32_t bx = 0;
                std::memcpy(&bx, bytes.data(), 4);  // little-endian hosts (x64)
                CHECK(bx == qcode(gp.x));
            }
        }
        ot.take_move_acks();
    }
    // right click on the hostile 9 -> Attack{9, attackers = selection}
    w.screen({-300, 200, 0}, x, y);
    mark = s.frames.size();
    s.click(Button::Right, x, y);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && out[0].type() == commands::EventType::Attack);
    if (out.size() == 1) {
        CHECK(out[0].u("target") == 9 && (out[0].ids("attackers") == std::vector<uint32_t>{3, 5}));
        CHECK(out[0].u("flag0") == 0 && out[0].u("ref2") == 0);
    }
    // right click on own 11 -> MoveToObject{11, movers = Sel (empty)}
    w.screen({150, -150, 0}, x, y);
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Right, x, y);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && out[0].type() == commands::EventType::MoveToObject);
    if (out.size() == 1) CHECK(out[0].u("target") == 11 && out[0].ids("movers").empty());
    // right drag on the ground (space) -> MoveToRayFacing{anchor ray origin, anchor plane point, angle, flags}
    mark = s.frames.size();
    s.move(500, 400);
    s.down(Button::Right);
    s.move(560, 420);
    s.up(Button::Right);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && out[0].type() == commands::EventType::MoveToRayFacing);
    if (out.size() == 1) {
        Vec3 a, b;
        ray_plane_z(pixel_ray(c.view, 500, 400), 0.0f, a);
        ray_plane_z(pixel_ray(c.view, 560, 420), 0.0f, b);
        const Vec3 o = pixel_ray(c.view, 500, 400).origin;
        CHECK(out[0].u("ax") == qcode(o.x) && out[0].u("ay") == qcode(o.y) && out[0].u("bx") == qcode(a.x) &&
              out[0].u("by") == qcode(a.y) && out[0].u("bz") == qcode(0.0f) && out[0].u("flags") == 0x04);
        const float want = std::atan2(b.y - a.y, b.x - a.x) * 57.29578f;
        CHECK(std::fabs(out[0].f("angle") - want) <= 0.1f);
    }
    ot.take_move_acks();
    // box select over everything of player 0 (enemy 9 excluded), ascending ids
    mark = s.frames.size();
    s.move(5, 5);
    s.down(Button::Left);
    s.move(795, 595);
    s.up(Button::Left);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && (out[0].ids("ids") == std::vector<uint32_t>{3, 5, 11}));
    // left click on empty ground deselects
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Left, 790, 10);
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 1 && out[0].ids("ids").empty() && ot.selection().empty());
    // nothing selected: right click emits nothing
    mark = s.frames.size();
    s.click(Button::Right, 650, 150);
    feed(in, ot, keys, c, s, mark);
    CHECK(ot.pending().empty());

    // double click with type_of: every on-screen own unit of the same type
    OrderContext c2 = c;
    c2.type_of = [](uint32_t id) { return id == 11 ? 2u : 1u; };
    w.screen({0, 0, 0}, x, y);
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Left, x, y);
    s.click(Button::Left, x, y);
    feed(in, ot, keys, c2, s, mark);
    out = ot.take();
    CHECK(out.size() == 2 && (out[1].ids("ids") == std::vector<uint32_t>{3, 5}));

    // keys: SelectAll (Ctrl+Y [port: Gabriel]), Stop (H since W A S D move [port: Gabriel]), control groups (3 recall, Ctrl+4 assign, Ctrl+Shift+5 add)
    mark = s.frames.size();
    s.key(vk::kControl, true);
    s.tap(vk::key('Y'));
    s.tap(static_cast<uint16_t>(vk::k0 + 4));
    s.key(vk::kShift, true);
    s.tap(static_cast<uint16_t>(vk::k0 + 5));
    s.key(vk::kShift, false);
    s.key(vk::kControl, false);
    s.tap(static_cast<uint16_t>(vk::k0 + 3));
    s.tap(vk::key('H'));
    feed(in, ot, keys, c, s, mark);
    out = ot.take();
    CHECK(out.size() == 5);
    if (out.size() == 5) {
        CHECK(out[0].type() == commands::EventType::SelectAll);
        CHECK(out[1].type() == commands::EventType::ControlGroup && out[1].u("group") == 4 && out[1].u("assign") == 1 &&
              out[1].u("add") == 0);
        CHECK(out[2].u("group") == 5 && out[2].u("assign") == 1 && out[2].u("add") == 1);
        CHECK(out[3].u("group") == 3 && out[3].u("assign") == 0 && out[3].u("add") == 0);
        CHECK(out[4].type() == commands::EventType::StopMovement);
        for (const auto& e : out) CHECK(e.valid() && commands::event_scheduled(e.type_id()));
    }

    // Select is capped at 127 ids (list7), lowest kept
    std::vector<uint32_t> many;
    for (uint32_t i = 0; i < 200; ++i) many.push_back(1000 - i);
    ot.set_selection(many);
    CHECK(ot.selection().size() == 200 && ot.selection().front() == 801);
    {
        tactical::TreeSet big(1);
        for (uint32_t i = 0; i < 200; ++i) big.add(100 + i, cube(static_cast<float>(i % 20) * 10.0f - 100.0f,
                                                                   static_cast<float>(i / 20) * 10.0f - 50.0f, 0, 2), 1, {0});
        OrderContext cb = c;
        cb.trees = &big;
        mark = s.frames.size();
        s.move(1, 1);
        s.down(Button::Left);
        s.move(798, 598);
        s.up(Button::Left);
        feed(in, ot, keys, cb, s, mark);
        out = ot.take();
        CHECK(out.size() == 1 && out[0].ids("ids").size() == 127 && out[0].ids("ids").front() == 100 &&
              out[0].ids("ids").back() == 226 && out[0].valid());
    }

    // galactic: click fleet selects, right click planet moves each selected fleet, double click planet zooms
    GalacticCameraController gcam;
    OrderContext gc;
    gc.mode = UiMode::Galactic;
    gc.view = make_view(gcam.params(800.0f / 600.0f), 800, 600);
    const PickSphere items[] = {{40, {0, 0, 0}, 15, 0, PickKind::Fleet}, {70, {200, 0, 0}, 30, -1, PickKind::Planet}};
    gc.galactic_items = items;
    OrderTranslator got;
    float fx = 0, fy = 0, px = 0, py = 0;
    project(gc.view, {0, 0, 0}, fx, fy);
    project(gc.view, {200, 0, 0}, px, py);
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Left, static_cast<int32_t>(fx), static_cast<int32_t>(fy));
    s.click(Button::Right, static_cast<int32_t>(px), static_cast<int32_t>(py));
    feed(in, got, keys, gc, s, mark);
    out = got.take();
    CHECK(out.size() == 2);
    if (out.size() == 2) {
        CHECK((out[0].ids("ids") == std::vector<uint32_t>{40}));
        CHECK(out[1].type() == commands::EventType::MoveObjectToObject && out[1].u("object") == 40 &&
              out[1].u("target") == 70);
    }
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Left, static_cast<int32_t>(px), static_cast<int32_t>(py));
    s.click(Button::Left, static_cast<int32_t>(px), static_cast<int32_t>(py));
    feed(in, got, keys, gc, s, mark);
    CHECK(got.take_planet_zoom() == std::optional<uint32_t>(70) && got.pending().empty());

    // submit through the pipeline: scheduled for the next tick
    commands::CommandPipeline pipe;
    ot.set_selection({3});
    mark = s.frames.size();
    for (int i = 0; i < 40; ++i) s.idle();
    s.click(Button::Right, 650, 150);
    feed(in, ot, keys, c, s, mark);
    CHECK(ot.submit(pipe, 0) == 1 && ot.pending().empty());
    pipe.set_handler([](uint64_t, const commands::Command&) { return commands::RejectReason::None; });
    CHECK(pipe.begin_tick(0).commands.empty());
    const auto& batch = pipe.begin_tick(1);
    CHECK(batch.commands.size() == 1 && batch.commands[0].payload.type() == commands::EventType::MoveToRay);  // space
}

// ---- replay determinism ------------------------------------------------------------------------------
struct SessionResult {
    std::vector<uint8_t> batches;  // canonical batch bytes of every tick
    std::vector<uint32_t> camera;  // float bit patterns of the camera params each frame
    bool operator==(const SessionResult&) const = default;
};

SessionResult run_session(const std::vector<InputFrame>& frames) {
    World w;
    Bindings keys;
    InputState in;
    OrderTranslator ot;
    commands::CommandPipeline pipe;
    std::vector<uint32_t> synced_sel;  // the "sim" side of the selection
    pipe.set_handler([&](uint64_t, const commands::Command& cmd) {
        if (cmd.payload.type() == commands::EventType::Select) synced_sel = cmd.payload.ids("ids");
        return commands::RejectReason::None;
    });
    SessionResult r;
    uint64_t tick = 0;
    for (const InputFrame& f : frames) {
        in.update(f);  // pumped once per iteration, before the tick
        w.cam.update(in, keys, f.dt);
        OrderContext c = w.ctx();
        ot.update(in, keys, c);
        ot.submit(pipe, 0);
        const auto& b = pipe.begin_tick(tick++);
        commands::encode_batch(b, r.batches);
        ot.set_selection(synced_sel);
        const CameraParams p = w.cam.params(800.0f / 600.0f);
        for (float v : {p.eye.x, p.eye.y, p.eye.z, p.target.x, p.target.y, p.fov_x, p.distance}) {
            uint32_t u = 0;
            std::memcpy(&u, &v, 4);
            r.camera.push_back(u);
        }
    }
    return r;
}

// ---- P1 double-click move / move flags (P1_DOUBLE_CLICK_MOVE.md) ---------------------------------------------------
void test_double_click_move() {
    using T = commands::EventType;
    World w;
    OrderContext land = w.ctx();
    land.mode = UiMode::Land;
    const OrderContext space = w.ctx();
    Bindings keys;
    InputState in;
    OrderTranslator ot;
    ot.set_selection({3, 5});
    Script s;
    std::vector<MoveAck> acks;
    // runs the frames from `mark`, returns this batch's orders (acks -> `acks`)
    auto run = [&](const OrderContext& c, size_t mark) {
        feed(in, ot, keys, c, s, mark);
        acks = ot.take_move_acks();
        return ot.take_orders();
    };
    auto pause = [&] {
        for (int i = 0; i < 40; ++i) s.idle();
    };
    auto dbl = [&](int32_t x, int32_t y) {
        s.click(Button::Right, x, y);
        s.down(Button::Right);
        s.up(Button::Right);
    };

    // input: press-time Win32 double click inside the 4 x 4 rectangle (+-2 px); a third press starts a new pair
    {
        InputState is;
        Script t;
        t.click(Button::Right, 100, 100);  // frames 0 move, 1 down, 2 up
        t.move(102, 98);                   // 3
        t.down(Button::Right);             // 4
        t.up(Button::Right);               // 5
        t.down(Button::Right);             // 6
        bool seen_double = false, third_double = true, release_in_double = false;
        for (size_t i = 0; i < t.frames.size(); ++i) {
            is.update(t.frames[i]);
            const ButtonState& b = is.button(Button::Right);
            if (i == 4) seen_double = b.double_press && b.in_double_press;
            if (i == 5) release_in_double = b.released && b.in_double_press;
            if (i == 6) third_double = b.double_press || b.in_double_press;
        }
        CHECK(seen_double && release_in_double && !third_double);
        Script u;
        u.click(Button::Right, 100, 100);
        u.move(103, 100);  // outside +-2
        u.down(Button::Right);
        InputState is2;
        bool d = true;
        for (const InputFrame& f : u.frames) {
            is2.update(f);
            d = is2.button(Button::Right).double_press;
        }
        CHECK(!d);
        // a press of another button in between breaks the pair
        Script v;
        v.click(Button::Right, 100, 100);
        v.down(Button::Left);
        v.up(Button::Left);
        v.down(Button::Right);
        InputState is3;
        d = true;
        for (const InputFrame& f : v.frames) {
            is3.update(f);
            d = is3.button(Button::Right).double_press;
        }
        CHECK(!d);
        // the configured rectangle / time are honoured
        InputConfig cfg;
        cfg.double_click_rect_w = cfg.double_click_rect_h = 8;
        InputState is4(cfg);
        d = false;
        for (const InputFrame& f : u.frames) {
            is4.update(f);
            d = is4.button(Button::Right).double_press;
        }
        CHECK(d);
        const InputConfig sys = system_input_config();
        CHECK(sys.double_click_time > 0.0 && sys.double_click_rect_w > 0 && sys.double_click_rect_h > 0);
    }

    // 1. land right double click on ground: 0x04 move + voice + normal ack, then 0x84 at the press point, no voice,
    //    double-click ack; the swallowed release sends nothing.
    size_t mark = s.frames.size();
    dbl(650, 150);
    std::vector<UiOrder> o = run(land, mark);
    Vec3 gp;
    CHECK(ray_plane_z(pixel_ray(land.view, 650, 150), 0.0f, gp));
    CHECK(o.size() == 2);
    if (o.size() == 2) {
        CHECK(o[0].event.type() == T::MoveToPosition && o[0].event.u("flags") == 0x04 && o[0].voice && !o[0].double_click &&
              !o[0].queued);
        CHECK(o[1].event.type() == T::MoveToPosition && o[1].event.u("flags") == 0x84 && !o[1].voice && o[1].double_click &&
              !o[1].queued);
        CHECK(o[1].event.u("x") == qcode(gp.x) && o[1].event.u("y") == qcode(gp.y));
    }
    CHECK(acks.size() == 2);
    if (acks.size() == 2) {
        CHECK(acks[0].effect() == AckEffect::Move && !acks[0].kill_nearest_move_ack() && acks[0].radar() == AckRadar::Click &&
              !acks[0].space);
        CHECK(acks[1].effect() == AckEffect::DoubleClickMove && acks[1].kill_nearest_move_ack() &&
              acks[1].radar() == AckRadar::DoubleClick && acks[1].remove_click_radar());
        CHECK(near(acks[1].point.x, gp.x) && near(acks[1].point.y, gp.y));
        CHECK(ack_effect_constant(acks[1].effect()) == "GUI_Double_Click_Move_Command_Ack_Effect");
        CHECK(ack_radar_constant(acks[1].radar()) == "GUI_Movement_Double_Click_Radar_Event_Name");
        CHECK(ack_effect_constant(acks[0].effect()) == "GUI_Move_Command_Ack_Effect");
    }

    // 2. the double-click press dragged while held: still the 0x84 move at the press, never a facing drag; release dropped
    pause();
    mark = s.frames.size();
    s.click(Button::Right, 600, 200);
    s.down(Button::Right);
    s.move(640, 230);
    s.up(Button::Right);
    o = run(land, mark);
    CHECK(o.size() == 2 && o[1].event.type() == T::MoveToPosition && o[1].event.u("flags") == 0x84);
    if (o.size() == 2) {
        Vec3 p;
        ray_plane_z(pixel_ray(land.view, 600, 200), 0.0f, p);
        CHECK(o[1].event.u("x") == qcode(p.x));
    }

    // 3. Ctrl -> attack-move 0x05 / 0x85 (attack ack, no double kill); Ctrl+Alt -> guard 0x06 / 0x86
    pause();
    s.key(vk::kControl, true);
    mark = s.frames.size();
    dbl(650, 150);
    o = run(land, mark);
    CHECK(o.size() == 2 && o[0].event.u("flags") == 0x05 && o[1].event.u("flags") == 0x85);
    CHECK(acks.size() == 2 && acks[1].effect() == AckEffect::AttackMove && !acks[1].kill_nearest_move_ack() &&
          acks[1].radar() == AckRadar::AttackClick);
    pause();
    s.key(vk::kAlt, true);
    mark = s.frames.size();
    dbl(650, 150);
    o = run(land, mark);
    CHECK(o.size() == 2 && o[0].event.u("flags") == 0x06 && o[1].event.u("flags") == 0x86);
    CHECK(acks.size() == 2 && acks[1].effect() == AckEffect::GuardMove && acks[1].radar() == AckRadar::DoubleClick);
    s.key(vk::kControl, false);

    // 4. Alt alone: the first order starts the path (0x04), later ones append (0x0C, queued); Alt + double click appends
    //    too (0x8C); releasing Alt ends the run.
    pause();
    mark = s.frames.size();
    s.click(Button::Right, 500, 200);
    pause();
    s.click(Button::Right, 550, 250);
    pause();
    dbl(600, 300);
    s.key(vk::kAlt, false);
    pause();
    s.click(Button::Right, 500, 200);
    o = run(land, mark);
    CHECK(o.size() == 5);
    if (o.size() == 5) {
        CHECK(o[0].event.u("flags") == 0x04 && !o[0].queued);
        CHECK(o[1].event.u("flags") == 0x0C && o[1].queued);
        CHECK(o[2].event.u("flags") == 0x0C && o[3].event.u("flags") == 0x8C && o[3].queued);
        CHECK(o[4].event.u("flags") == 0x04 && !o[4].queued);
    }
    CHECK(!ot.waypoint_mode());

    // 5. [port: Gabriel] the queue modifier (Shift) appends: 0x0C, queued
    pause();
    s.key(vk::kShift, true);
    mark = s.frames.size();
    s.click(Button::Right, 500, 200);
    s.key(vk::kShift, false);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.u("flags") == 0x0C && o[0].queued);

    // 6. one-shot HUD toggles: attack-move applies to the next move only; guard wins over attack-move
    ot.arm_move_toggle(MoveToggle::AttackMove);
    pause();
    mark = s.frames.size();
    s.click(Button::Right, 500, 200);
    pause();
    s.click(Button::Right, 500, 200);
    o = run(land, mark);
    CHECK(o.size() == 2 && o[0].event.u("flags") == 0x05 && o[1].event.u("flags") == 0x04);
    ot.arm_move_toggle(MoveToggle::GuardMove);
    ot.arm_move_toggle(MoveToggle::AttackMove);
    pause();
    mark = s.frames.size();
    s.click(Button::Right, 500, 200);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.u("flags") == 0x06);

    // 7. land drag: Chebyshev 3 px (Euclid 4.2 > 4) is still a click; 60 px -> MoveToPositionFacing at the anchor;
    //    a drag starting on a selected unit -> Facing (rotate only).
    pause();
    mark = s.frames.size();
    s.move(500, 400);
    s.down(Button::Right);
    s.move(503, 403);
    s.up(Button::Right);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.type() == T::MoveToPosition);
    if (o.size() == 1) {
        Vec3 p;
        ray_plane_z(pixel_ray(land.view, 503, 403), 0.0f, p);
        CHECK(o[0].event.u("x") == qcode(p.x) && o[0].event.u("y") == qcode(p.y));
    }
    pause();
    mark = s.frames.size();
    s.move(500, 400);
    s.down(Button::Right);
    s.move(560, 420);
    s.up(Button::Right);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.type() == T::MoveToPositionFacing);
    if (o.size() == 1) {
        Vec3 a, b;
        ray_plane_z(pixel_ray(land.view, 500, 400), 0.0f, a);
        ray_plane_z(pixel_ray(land.view, 560, 420), 0.0f, b);
        CHECK(o[0].event.u("x") == qcode(a.x) && o[0].event.u("y") == qcode(a.y) && o[0].event.u("flags") == 0x04 &&
              o[0].event.ids("units").empty());
        CHECK(std::fabs(o[0].event.f("angle") - std::atan2(b.y - a.y, b.x - a.x) * 57.29578f) <= 0.1f);
        CHECK(acks.size() == 1 && near(acks[0].point.x, a.x));
    }
    int32_t ux = 0, uy = 0;
    w.screen({60, 0, 0}, ux, uy);
    pause();
    mark = s.frames.size();
    s.move(ux, uy);
    s.down(Button::Right);
    s.move(ux + 80, uy + 10);
    s.up(Button::Right);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.type() == T::Facing && o[0].event.ids("units").empty());

    // 8. double click on the hostile 9: Attack, then Attack with flag0 ("double") = 1, no voice; no move ack
    int32_t hx = 0, hy = 0;
    w.screen({-300, 200, 0}, hx, hy);
    pause();
    mark = s.frames.size();
    dbl(hx, hy);
    o = run(land, mark);
    CHECK(o.size() == 2 && acks.empty());
    if (o.size() == 2) {
        CHECK(o[0].event.type() == T::Attack && o[0].event.u("flag0") == 0 && o[0].voice);
        CHECK(o[1].event.type() == T::Attack && o[1].event.u("flag0") == 1 && o[1].double_click && !o[1].voice &&
              (o[1].event.ids("attackers") == std::vector<uint32_t>{3, 5}));
    }
    // Ctrl + click on the hostile: flag1 (+0x51 Ctrl)
    pause();
    s.key(vk::kControl, true);
    mark = s.frames.size();
    s.click(Button::Right, hx, hy);
    s.key(vk::kControl, false);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.u("flag1") == 1 && o[0].event.u("flag0") == 0);

    // 9. double click on the friendly 11: only the first click's MoveToObject
    w.screen({150, -150, 0}, ux, uy);
    pause();
    mark = s.frames.size();
    dbl(ux, uy);
    o = run(land, mark);
    CHECK(o.size() == 1 && o[0].event.type() == T::MoveToObject);

    // 10. space double click: MoveToRay 0x04 then 0x84 (space ack scale)
    pause();
    mark = s.frames.size();
    dbl(650, 150);
    o = run(space, mark);
    CHECK(o.size() == 2 && o[0].event.type() == T::MoveToRay && o[0].event.u("flags") == 0x04 &&
          o[1].event.type() == T::MoveToRay && o[1].event.u("flags") == 0x84);
    CHECK(acks.size() == 2 && acks[1].space && acks[1].effect() == AckEffect::DoubleClickMove);

    // 11. a triple click: the third press is a new first press -> a third normal order on its release
    pause();
    mark = s.frames.size();
    dbl(650, 150);
    s.down(Button::Right);
    s.up(Button::Right);
    o = run(land, mark);
    CHECK(o.size() == 3 && o[2].event.u("flags") == 0x04 && o[2].voice);

    // 12. nothing selected: no orders at all
    ot.set_selection({});
    pause();
    mark = s.frames.size();
    dbl(650, 150);
    o = run(land, mark);
    CHECK(o.empty() && acks.empty());
}

void test_replay() {
    Script s;
    s.idle();
    s.key(vk::kUp, true);
    for (int i = 0; i < 2; ++i) s.idle();
    s.key(vk::kUp, false);
    s.wheel(-1.0f);
    for (int i = 0; i < 10; ++i) s.idle();
    s.move(400, 300);
    s.down(Button::Middle);
    s.move(430, 290);
    s.up(Button::Middle);
    s.move(10, 10);
    s.down(Button::Left);
    s.move(790, 590);
    s.up(Button::Left);
    s.click(Button::Right, 300, 200);
    s.move(0, 300);  // edge scroll
    for (int i = 0; i < 5; ++i) s.idle();
    s.move(400, 300);
    s.key(vk::kControl, true);
    s.tap(static_cast<uint16_t>(vk::k0 + 2));
    s.key(vk::kControl, false);
    s.click(Button::Right, 450, 350);

    InputReplay rec;
    for (const InputFrame& f : s.frames) rec.record(f);
    const SessionResult live = run_session(rec.frames);
    CHECK(!live.batches.empty());

    const std::vector<uint8_t> bytes = rec.serialize();
    CHECK(bytes.size() == 12 + rec.frames.size() * kReplayFrameBytes);
    InputReplay back;
    CHECK(InputReplay::deserialize(bytes, back));
    CHECK(back.frames == rec.frames);
    const SessionResult replayed = run_session(back.frames);
    CHECK(replayed == live);
    CHECK(back.serialize() == bytes);

    // the session produced orders: Select (box), MoveToPosition, ControlGroup, MoveToPosition
    size_t pos = 0, cmds = 0;
    while (pos < live.batches.size()) {
        commands::CommandBatch b;
        if (commands::decode_batch(live.batches, pos, b) != commands::DecodeError::None) break;
        cmds += b.commands.size();
    }
    CHECK(pos == live.batches.size() && cmds == 4);

    // corrupt inputs are rejected
    InputReplay bad;
    std::vector<uint8_t> trunc(bytes.begin(), bytes.end() - 1);
    CHECK(!InputReplay::deserialize(trunc, bad) && bad.frames.empty());
    std::vector<uint8_t> magic = bytes;
    magic[0] = 'X';
    CHECK(!InputReplay::deserialize(magic, bad));
    std::vector<uint8_t> ver = bytes;
    ver[4] = 9;
    CHECK(!InputReplay::deserialize(ver, bad));
}

// ---- P1 completion: overview zoom, terrain following, control-group camera jump ------------------------------
void test_camera_p1() {
    // Overview: space (Distance 2200 / 2900, Clicks 10 in 1.8 s, Pitch -1 = keep, FOV 60 / 70).
    TacticalCameraController sp(space_mode_defaults());
    CHECK(sp.max_overview_level() == 2);
    sp.set_distance(1900);
    for (int i = 0; i < 9; ++i) {
        sp.zoom(-1.0f);
        sp.step(0.05f);
    }
    CHECK(sp.overview_level() == 0);
    sp.zoom(-1.0f);  // 10th notch at max within 1.8 s
    CHECK(sp.overview_level() == 1);
    CHECK(sp.overview_transitioning());
    for (int i = 0; i < 40; ++i) sp.step(0.05f);
    CHECK(!sp.overview_transitioning());
    CHECK(near(sp.distance(), 2200.0f));
    CHECK(near(sp.pitch_deg(), 50.0f));   // pitch -1: kept
    CHECK(near(sp.fov_deg(), 60.0f));
    // slow notches (beyond the click time) do not add up
    for (int i = 0; i < 12; ++i) {
        sp.zoom(-1.0f);
        sp.step(2.0f);
    }
    CHECK(sp.overview_level() == 1);
    for (int i = 0; i < 10; ++i) sp.zoom(-1.0f);
    CHECK(sp.overview_level() == 2);
    for (int i = 0; i < 40; ++i) sp.step(0.05f);
    CHECK(near(sp.distance(), 2900.0f) && near(sp.pitch_deg(), 80.0f) && near(sp.fov_deg(), 70.0f));
    // halfway through the blend back the pose is between the two
    sp.zoom(1.0f);
    CHECK(sp.overview_level() == 0);
    sp.step(0.25f);
    CHECK(sp.distance() < 2900.0f && sp.distance() > 1900.0f);
    for (int i = 0; i < 40; ++i) sp.step(0.05f);
    CHECK(near(sp.distance(), 1900.0f) && near(sp.fov_deg(), 55.0f));
    CHECK(near(sp.target_distance(), 1900.0f));  // the normal zoom is untouched by the overview
    // toggle (hotkey 76): 0 -> 1 -> 2 -> 0; params use the effective pose
    sp.toggle_overview();
    CHECK(sp.overview_level() == 1);
    sp.toggle_overview();
    CHECK(sp.overview_level() == 2);
    for (int i = 0; i < 40; ++i) sp.step(0.05f);
    const CameraParams po = sp.params(4.0f / 3.0f);
    CHECK(near(po.distance, 2900.0f, 1e-2f) && near(po.fov_x, 70.0f * kDegToRad, 1e-4f));
    sp.toggle_overview();
    CHECK(sp.overview_level() == 0);
    sp.reset();
    CHECK(sp.overview_level() == 0 && !sp.overview_transitioning());
    // zooming out below the max distance never counts
    sp.set_distance(1000);
    for (int i = 0; i < 3; ++i) sp.zoom(-1.0f);  // 1000 -> 1900 (target), clicks only at the max
    CHECK(sp.overview_level() == 0);

    // Land: overview 1000 / pitch 65 / fov 55 (4 clicks in 1.5 s), level 2 1600 / 80 / 70.
    TacticalCameraController land(land_mode_defaults());
    CHECK(land.max_overview_level() == 2);
    for (int i = 0; i < 4; ++i) land.zoom(-1.0f);  // already at Distance_Max 460
    CHECK(land.overview_level() == 1);
    for (int i = 0; i < 20; ++i) land.step(0.05f);
    CHECK(near(land.distance(), 1000.0f) && near(land.pitch_deg(), 65.0f));
    // Unlocked: 99 clicks within click time 0 -> only the toggle reaches the overview
    TacticalCameraController un(unlocked_defaults());
    un.set_distance(60000);
    for (int i = 0; i < 120; ++i) {
        un.zoom(-1.0f);
        un.step(0.01f);
    }
    CHECK(un.overview_level() == 0);

    // Terrain following: look-at height follows a slope with smoothing; jumps snap.
    TacticalCameraConstants lc = land_mode_defaults();
    TacticalCameraController tf(lc);
    tf.set_bounds({-2000, -2000, 2000, 2000});
    auto slope = [](float x, float /*y*/) { return 0.1f * x; };
    tf.set_look_at({100, 0, 0});
    tf.set_terrain_height(slope);
    CHECK(near(tf.look_at().z, 10.0f));
    tf.pan(100, 0);  // yaw 0: right = +X
    CHECK(near(tf.look_at().x, 200.0f));
    tf.step(0.125f);  // SmoothCD up: Location_Height_Up_Smooth_Time 0.3
    CHECK(near(tf.look_at().z, smooth_ref(10.0f, 0.0f, 20.0f, 0.3f, 0.125f), 1e-3f));
    settle(tf);
    CHECK(near(tf.look_at().z, 20.0f, 1e-3f));
    tf.pan(-100, 0);  // down: Location_Height_Down_Smooth_Time 1.0
    tf.step(0.125f);
    CHECK(near(tf.look_at().z, smooth_ref(20.0f, 0.0f, 10.0f, 1.0f, 0.125f), 1e-3f));
    settle(tf);
    tf.jump_to({-500, 300, 0});
    CHECK(near(tf.look_at().x, -500.0f) && near(tf.look_at().y, 300.0f) && near(tf.look_at().z, -50.0f));
    // jump clamps to the limits
    tf.jump_to({5000, 0, 0});
    CHECK(near(tf.look_at().x, 2000.0f));
    // Eye clearance: a wall behind the camera (y < look.y - 100) 500 high forces a steeper pitch.
    TacticalCameraController wall(land_mode_defaults());
    wall.set_bounds({-2000, -2000, 2000, 2000});
    wall.set_look_at({0, 0, 0});
    const CameraParams flat = wall.params(4.0f / 3.0f);
    wall.set_terrain_height([](float, float y) { return y < -100.0f ? 500.0f : 0.0f; });
    const CameraParams raised = wall.params(4.0f / 3.0f);
    CHECK(flat.eye.y < -100.0f && flat.eye.z < 520.0f);  // the default pose is inside the wall
    const float floor_z = (raised.eye.y < -100.0f ? 500.0f : 0.0f) + 20.0f;  // Min_Height_Above_Terrain 20
    CHECK(raised.eye.z >= floor_z - 0.05f);
    CHECK(raised.orbit_pitch > flat.orbit_pitch);
    CHECK(near(raised.distance, flat.distance));
    // consistent pose: eye = target + distance * (orbit direction)
    const float dz = raised.eye.z - raised.target.z;
    CHECK(near(dz, raised.distance * std::sin(raised.orbit_pitch), 1e-2f));
    // no callback / space: unchanged
    TacticalCameraController sp2(space_mode_defaults());
    const CameraParams a = sp2.params(1.5f);
    sp2.set_terrain_height([](float, float) { return 1e6f; });  // Location_Follows_Terrain 0, Min height 0
    const CameraParams b = sp2.params(1.5f);
    CHECK(a.eye.z == b.eye.z && sp2.look_at().z == 0.0f);

    // focus point = mean (index order)
    const Vec3 pts[3] = {{0, 0, 0}, {30, 60, 0}, {60, 0, 9}};
    const auto fp = focus_point(pts);
    CHECK(fp && near(fp->x, 30.0f) && near(fp->y, 20.0f) && near(fp->z, 3.0f));
    CHECK(!focus_point(std::span<const Vec3>{}));

    // OrderTranslator: recall twice within 1 s -> take_camera_jump(); assign does not count; slow recalls do not.
    OrderTranslator ot;
    Bindings keys;
    InputState in;
    Script s;
    OrderContext ctx;
    ctx.mode = UiMode::Space;
    s.tap(vk::k0 + 3);
    s.tap(vk::k0 + 3);
    for (const InputFrame& f : s.frames) {
        in.update(f);
        ot.update(in, keys, ctx);
    }
    auto j = ot.take_camera_jump();
    CHECK(j && *j == 3);
    CHECK(!ot.take_camera_jump());
    CHECK(ot.pending().size() == 2);
    s.frames.clear();
    for (int i = 0; i < 90; ++i) s.idle();  // 1.5 s
    s.tap(vk::k0 + 3);
    s.key(vk::kControl, true);
    s.tap(vk::k0 + 3);  // Ctrl+3 = assign
    s.key(vk::kControl, false);
    for (const InputFrame& f : s.frames) {
        in.update(f);
        ot.update(in, keys, ctx);
    }
    CHECK(!ot.take_camera_jump());
}

}  // namespace

int main() {
    test_input();
    test_camera();
    test_camera_p1();
    test_parse();
    test_picking();
    test_orders();
    test_double_click_move();
    test_replay();
    if (g_fail) {
        std::printf("alamo_ui_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_tests: all passed\n");
    return 0;
}
