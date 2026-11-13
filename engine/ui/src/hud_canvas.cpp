#include "alamo/ui/hud_canvas.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::ui {

std::array<std::array<float, 4>, 4> HudOrthoCamera::view_proj() const {
    // view: translate by -eye_z along Z (camera looks down -Z); proj: D3DXMatrixOrthoRH (depth 0 at near).
    const float range = zfar - znear;
    std::array<std::array<float, 4>, 4> m{};
    m[0] = {1.0f / half_width, 0, 0, 0};
    m[1] = {0, 1.0f / half_height, 0, 0};
    // view-space z_v = z - eye_z (negative in front); depth = (-z_v - near) / range.
    m[2] = {0, 0, -1.0f / range, (eye_z() - znear) / range};
    m[3] = {0, 0, 0, 1};
    return m;
}

HudCanvas HudCanvas::make(uint32_t screen_w, uint32_t screen_h, float ui_scale) {
    HudCanvas c;
    c.screen_width = screen_w ? screen_w : 1;
    c.screen_height = screen_h ? screen_h : 1;
    c.ui_scale = ui_scale > 0.0f ? ui_scale : 0.0f;
    c.text_height = c.screen_height;
    const auto sw = static_cast<double>(c.screen_width), sh = static_cast<double>(c.screen_height);
    double fw = kHudBaseWidth, fh = kHudBaseHeight;  // the fitted (original) canvas
    if (c.wider_than_4_3()) {
        fw = sw * 768.0 / sh;
    } else if (c.taller_than_4_3()) {
        fh = sh * 1024.0 / sw;
    }
    // UI scale: one canvas unit is ui_scale x kUiScaleUnitPixels pixels; the canvas is the screen / that when it is larger
    // than the fitted one (same aspect, so one factor).
    const double unit_px = static_cast<double>(c.ui_scale) * static_cast<double>(kUiScaleUnitPixels);
    const double raw = c.ui_scale > 0.0f ? sh / (unit_px * fh) : 1.0;
    const double k = std::max(1.0, raw);
    c.width = static_cast<float>(fw * k);
    c.height = static_cast<float>(fh * k);
    if (k > 1.0) {
        c.text_height = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(sh / k)));
    } else if (raw < 1.0) {
        // Past the original size: the layout stays fitted, only the text grows, by the scale over the one that reaches the
        // original size (100 % when a smaller one already does, so 100 % never enlarges text), at most kUiScaleMaxTextGrowth.
        const double fit_scale = std::max(1.0, sh / (static_cast<double>(kUiScaleUnitPixels) * fh));
        const double grow = std::min(static_cast<double>(c.ui_scale) / fit_scale, static_cast<double>(kUiScaleMaxTextGrowth));
        if (grow > 1.0) c.text_height = static_cast<uint32_t>(std::lround(sh * grow));
    }
    c.scale_x = c.width / kHudBaseWidth;
    c.scale_y = c.height / kHudBaseHeight;
    c.units_per_pixel_x = c.width / static_cast<float>(c.screen_width);
    c.units_per_pixel_y = c.height / static_cast<float>(c.screen_height);
    const float hw = 0.5f * c.width, hh = 0.5f * c.height;
    c.corners = {Vec2f{-hw, hh}, Vec2f{hw, hh}, Vec2f{hw, -hh}, Vec2f{-hw, -hh}};
    const float nears[kHudCameraCount] = {0.1f, 0.01f, 0.01f}, fars[kHudCameraCount] = {40.0f, 500.0f, 40.0f};
    for (size_t i = 0; i < kHudCameraCount; ++i) c.cameras[i] = HudOrthoCamera{hw, hh, nears[i], fars[i]};
    return c;
}

Vec2f HudCanvas::pixel_to_canvas(float px, float py) const {
    return {(px - 0.5f * static_cast<float>(screen_width)) * units_per_pixel_x,
            (0.5f * static_cast<float>(screen_height) - py) * units_per_pixel_y};
}

Vec2f HudCanvas::canvas_to_pixel(float cx, float cy) const {
    return {cx / units_per_pixel_x + 0.5f * static_cast<float>(screen_width),
            0.5f * static_cast<float>(screen_height) - cy / units_per_pixel_y};
}

Vec2f HudCanvas::snap_to_pixel(Vec2f c) const {
    const Vec2f p = canvas_to_pixel(c.x, c.y);
    return pixel_to_canvas(std::floor(p.x + 0.5f), std::floor(p.y + 0.5f));
}

float font_pixel_height(float point_size, uint32_t screen_height) {
    return point_size * (static_cast<float>(screen_height) * 96.0f / 600.0f) / 72.0f;
}

int32_t font_pixel_height_int(float point_size, uint32_t screen_height) {
    const auto h = static_cast<int32_t>(std::floor(font_pixel_height(point_size, screen_height) + 0.5f));
    return h < 1 ? 1 : h;
}

}  // namespace alamo::ui
