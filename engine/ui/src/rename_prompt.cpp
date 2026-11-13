// [port: Gabriel, 2026-11-13] Fleet / ship rename prompt (rename_prompt.hpp).
#include "alamo/ui/rename_prompt.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/commands/command.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

std::u32string u32(std::string_view ascii) { return std::u32string(ascii.begin(), ascii.end()); }

void rect(HudDrawList& dl, const HudCanvas& c, float x0, float y0, float x1, float y1, Rgba8 color) {
    HudQuad q;
    q.min = c.pixel_to_canvas(x0, y1);
    q.max = c.pixel_to_canvas(x1, y0);
    q.color = color;
    dl.add(kHudLayers - 1, HudList::Alpha, q);
}

}  // namespace

void RenamePrompt::open(uint32_t object, bool fleet, std::string_view current) {
    active_ = true;
    object_ = object;
    fleet_ = fleet;
    text_ = utf8_to_utf32(current);
    if (text_.size() > commands::kMaxObjectNameChars) text_.resize(commands::kMaxObjectNameChars);
}

std::string RenamePrompt::utf8() const {
    std::string o;
    for (const char32_t c : text_) {
        const auto v = static_cast<uint32_t>(c);
        if (v < 0x80) {
            o.push_back(static_cast<char>(v));
        } else if (v < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (v >> 6)));
            o.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        } else if (v < 0x10000) {
            o.push_back(static_cast<char>(0xE0 | (v >> 12)));
            o.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xF0 | (v >> 18)));
            o.push_back(static_cast<char>(0x80 | ((v >> 12) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        }
    }
    return o;
}

RenamePrompt::Result RenamePrompt::update(std::u32string_view typed, bool backspace, bool enter, bool escape) {
    if (!active_) return Result::None;
    if (escape) {
        active_ = false;
        return Result::Cancel;
    }
    for (const char32_t c : typed) {
        const auto v = static_cast<uint32_t>(c);
        // Printable only (the host's printable() already drops controls); no surrogates, nothing past U+10FFFF.
        if (v < 0x20 || (v >= 0x7F && v <= 0x9F) || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) continue;
        if (text_.size() >= commands::kMaxObjectNameChars) break;
        text_.push_back(c);
    }
    if (backspace && !text_.empty()) text_.pop_back();
    if (enter) {
        active_ = false;
        return Result::Submit;
    }
    return Result::None;
}

void RenamePrompt::build(HudDrawList& dl, FontCache& fonts, const HudCanvas& c, double now) const {
    if (!active_) return;
    HudTextStyle st;
    st.font = fonts.font("Arial", 9.0f, c.screen_height);
    if (st.font < 0) return;
    st.color = {235, 235, 235, 255};
    st.outline = true;
    st.align = TextAlign::Center;
    const float lh = static_cast<float>(std::max(fonts.metrics(st.font).line_height, 12));
    const float w = std::min(460.0f, static_cast<float>(c.screen_width) - 32.0f), pad = 10.0f;
    const float h = pad * 2.0f + lh * 3.0f + 8.0f;
    const float cx = static_cast<float>(c.screen_width) * 0.5f;
    const float x0 = cx - w * 0.5f, y0 = static_cast<float>(c.screen_height) * 0.22f;
    rect(dl, c, x0 - 2.0f, y0 - 2.0f, x0 + w + 2.0f, y0 + h + 2.0f, {150, 170, 200, 200});  // rim
    rect(dl, c, x0, y0, x0 + w, y0 + h, {10, 14, 22, 235});
    const auto line = [&](int row, const std::u32string& text, Rgba8 color) {
        HudTextStyle s = st;
        s.color = color;
        const float y = y0 + pad + (static_cast<float>(row) + 0.5f) * lh + (row >= 1 ? 4.0f : 0.0f) + (row >= 2 ? 4.0f : 0.0f);
        add_text(dl, kHudLayers - 1, HudList::Text, fonts, c, text, c.pixel_to_canvas(cx, y), s);
    };
    line(0, u32(fleet_ ? "Rename fleet" : "Rename ship"), {255, 220, 120, 255});
    // The text field: a darker strip with the text and a caret blinking at 2 Hz (wall clock, presentation only).
    const float fy = y0 + pad + lh + 4.0f;
    rect(dl, c, x0 + pad, fy - 2.0f, x0 + w - pad, fy + lh + 2.0f, {0, 0, 0, 255});
    const bool caret = std::fmod(now, 1.0) < 0.5;
    std::u32string shown = text_;
    shown += caret ? U'|' : U' ';
    line(1, shown, {255, 255, 255, 255});
    line(2, u32("Enter: save   Esc: cancel   empty: default name"), {170, 180, 195, 255});
}

}  // namespace alamo::ui
