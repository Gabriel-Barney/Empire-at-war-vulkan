// P2 HUD canvas (docs/re/UI.md §2.4, §1.2, §3.4): the 1024 x 768 virtual canvas every HUD layer is laid
// out in, its Hor+ widening, the three orthographic HUD cameras rebuilt on every resolution change, the
// pixel <-> canvas conversions and the font pixel-size rule.
//
// Canvas units [H]: base 1024 x 768, origin at the screen centre. Aspect > 4:3: width = screenW * 768 / screenH
// (height stays 768); aspect < 4:3: height = screenH * 1024 / screenW (width stays 1024). Scale = canvas size /
// base size; units per pixel = canvas size / screen pixels.
// [port] axis convention: canvas +X right, +Y UP (the ortho camera's convention, like world space); pixel
// coordinates are top-left origin, +Y down (InputFrame / picking). Component offsets from
// CommandBarComponents.xml (Offset, Text_Offset, ...) are read as canvas units with +Y DOWN (screen
// convention: st_control_group "0 -32" puts the number above the unit) - see hud_world.hpp.
#pragma once
#include <array>
#include <cstdint>

namespace alamo::ui {

inline constexpr float kHudBaseWidth = 1024.0f, kHudBaseHeight = 768.0f;

struct Vec2f {
    float x = 0, y = 0;
    friend bool operator==(const Vec2f&, const Vec2f&) = default;
};

// One HUD orthographic camera (UI.md §2.4): spans +-half_width x +-half_height, near / far planes.
struct HudOrthoCamera {
    float half_width = kHudBaseWidth * 0.5f, half_height = kHudBaseHeight * 0.5f;
    float znear = 0.1f, zfar = 40.0f;
    // [port] the camera sits at z = eye_z looking down -Z (models at z = 0 are mid-range): eye_z = (near + far) / 2.
    float eye_z() const { return 0.5f * (znear + zfar); }
    // Row-major (column-vector) view-projection, D3D depth 0..1, clip Y up: x_clip = x / half_width,
    // y_clip = y / half_height, depth = (eye_z - z - near) / (far - near).
    std::array<std::array<float, 4>, 4> view_proj() const;
};

enum HudCameraId : uint8_t {
    kHudCameraComponents = 0,  // +0xfc0: near 0.1, far 40 (component quads, text)
    kHudCameraShells = 1,      // +0xfd0: near 0.01, far 500 (3D shell models)
    kHudCameraModels2 = 2,     // +0xfe0: near 0.01, far 40 (second model set)
    kHudCameraCount = 3
};

struct HudCanvas {
    uint32_t screen_width = 1024, screen_height = 768;
    float width = kHudBaseWidth, height = kHudBaseHeight;  // canvas units
    float scale_x = 1.0f, scale_y = 1.0f;                 // width / 1024, height / 768
    float units_per_pixel_x = 1.0f, units_per_pixel_y = 1.0f;
    std::array<Vec2f, 4> corners{};  // canvas units: top-left, top-right, bottom-right, bottom-left
    std::array<HudOrthoCamera, kHudCameraCount> cameras{};

    // Rebuilt on every resolution change (WM_SIZE in the original). 0 sizes are treated as 1.
    static HudCanvas make(uint32_t screen_w, uint32_t screen_h);
    bool wider_than_4_3() const { return static_cast<uint64_t>(screen_width) * 3u > static_cast<uint64_t>(screen_height) * 4u; }
    bool taller_than_4_3() const { return static_cast<uint64_t>(screen_width) * 3u < static_cast<uint64_t>(screen_height) * 4u; }

    // Pixel (top-left origin, +Y down; pixel centres at +0.5) <-> canvas (centre origin, +Y up).
    Vec2f pixel_to_canvas(float px, float py) const;
    Vec2f canvas_to_pixel(float cx, float cy) const;
    // Snap a canvas point to the pixel grid (text, Pixel_Align): round the pixel position (half-pixel bias).
    Vec2f snap_to_pixel(Vec2f c) const;
    // Pixel lengths -> canvas lengths.
    float px_to_units_x(float px) const { return px * units_per_pixel_x; }
    float px_to_units_y(float py) const { return py * units_per_pixel_y; }
    // Wrapped text width factor: 0.95 at exactly 800 pixels wide, else 1 (UI.md §2.4, 0x2df0c0).
    float text_wrap_factor() const { return screen_width == 800 ? 0.95f : 1.0f; }
};

// Font em height in pixels for a point size at a screen height (UI.md §3.4 [H]):
// point * (screenH * 96 / 600) / 72. Returned unrounded; font_pixel_height_int rounds to nearest [port]
// (CreateFont takes an int; rounding mode not in the doc), at least 1.
float font_pixel_height(float point_size, uint32_t screen_height);
int32_t font_pixel_height_int(float point_size, uint32_t screen_height);

}  // namespace alamo::ui
