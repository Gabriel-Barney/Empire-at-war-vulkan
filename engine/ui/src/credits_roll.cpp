#include "alamo/ui/credits_roll.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/data/game_database.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

uint32_t rd32(std::span<const uint8_t> b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// "r, g, b" in 0..1 -> RGBA8 (alpha 255); a missing / malformed value keeps `fallback`.
Rgba8 unit_color(const data::GameConstants& gc, std::string_view tag, Rgba8 fallback) {
    const auto v = gc.get(tag);
    if (!v) return fallback;
    float c[3] = {0, 0, 0};
    size_t n = 0;
    std::string s(*v);
    for (char& ch : s)
        if (ch == ',') ch = ' ';
    const char* p = s.c_str();
    char* end = nullptr;
    for (; n < 3; ++n) {
        const float f = std::strtof(p, &end);
        if (end == p) break;
        c[n] = f;
        p = end;
    }
    if (n < 3) return fallback;
    auto u8 = [](float f) { return static_cast<uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    return {u8(c[0]), u8(c[1]), u8(c[2]), 255};
}

}  // namespace

CreditsStyle credits_style(const data::GameConstants& gc) {
    CreditsStyle s;
    if (const auto f = gc.get("Credits_Font"); f && !f->empty()) s.font = std::string(*f);
    s.font_size = gc.get_float("Credits_Font_Size", s.font_size);
    s.header_top = unit_color(gc, "Credits_Header_Top_Color", s.header_top);
    s.header_bottom = unit_color(gc, "Credits_Header_Bottom_Color", s.header_bottom);
    s.top = unit_color(gc, "Credits_Top_Color", s.top);
    s.bottom = unit_color(gc, "Credits_Bottom_Color", s.bottom);
    s.spacing = std::max(0.005f, gc.get_float("Credits_Spacing", s.spacing));
    s.margin = std::clamp(gc.get_float("Credits_Margin", s.margin), 0.0f, 0.45f);
    s.scroll_rate = gc.get_float("Credits_Scroll_Rate", s.scroll_rate);
    if (std::fabs(s.scroll_rate) < 0.005f) s.scroll_rate = -0.10f;
    return s;
}

bool parse_credits(std::span<const uint8_t> b, std::vector<CreditsLine>& out) {
    out.clear();
    if (b.size() < 4) return false;
    const uint32_t n = rd32(b, 0);
    if (n > (b.size() - 4) / 12) return false;
    size_t values = 4 + static_cast<size_t>(n) * 12, total_chars = 0, total_keys = 0;
    for (uint32_t i = 0; i < n; ++i) {
        total_chars += rd32(b, 4 + 12 * i + 4);
        total_keys += rd32(b, 4 + 12 * i + 8);
    }
    if (values + total_chars * 2 + total_keys > b.size()) return false;
    size_t keys = values + total_chars * 2;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t vl = rd32(b, 4 + 12 * i + 4), kl = rd32(b, 4 + 12 * i + 8);
        std::u32string text;
        text.reserve(vl);
        for (uint32_t c = 0; c < vl; ++c) {
            const uint16_t u = static_cast<uint16_t>(b[values + 2 * c] | (b[values + 2 * c + 1] << 8));
            text.push_back(static_cast<char32_t>(u));  // the credits are BMP text
        }
        values += static_cast<size_t>(vl) * 2;
        const std::string key(reinterpret_cast<const char*>(b.data() + keys), kl);
        keys += kl;
        CreditsLine l;
        if (text == U"[TBL]" || text.empty()) {
            l.kind = CreditsLine::Kind::Blank;
        } else {
            l.kind = key == "HEADER" ? CreditsLine::Kind::Header : CreditsLine::Kind::Name;
            l.text = std::move(text);
        }
        out.push_back(std::move(l));
    }
    return true;
}

std::string credits_path(std::string_view language) { return "Data\\Text\\CreditsText_" + std::string(language) + ".DAT"; }

void CreditsRoll::start(std::vector<CreditsLine> lines, const CreditsStyle& style, double now) {
    lines_ = std::move(lines);
    style_ = style;
    t0_ = now;
    active_ = !lines_.empty();
    drawn_ = 0;
}

double CreditsRoll::duration() const {
    // From the first line entering at the bottom to the last leaving at the top: (1 + lines x spacing) screen heights.
    const double h = 1.0 + static_cast<double>(lines_.size() + 1) * static_cast<double>(style_.spacing);
    return h / std::max(0.005, std::fabs(static_cast<double>(style_.scroll_rate)));
}

bool CreditsRoll::build(HudDrawList& dl, FontCache& fonts, const HudCanvas& cv, double now) {
    drawn_ = 0;
    if (!active_) return false;
    const double t = std::max(0.0, now - t0_);
    if (t > duration()) {
        active_ = false;
        return false;
    }
    const float sw = static_cast<float>(cv.screen_width), sh = static_cast<float>(cv.screen_height);
    // The black backdrop over the menu.
    HudQuad bg;
    const Vec2f a = cv.pixel_to_canvas(0.0f, 0.0f), b = cv.pixel_to_canvas(sw, sh);
    bg.min = {std::min(a.x, b.x), std::min(a.y, b.y)};
    bg.max = {std::max(a.x, b.x), std::max(a.y, b.y)};
    bg.color = {0, 0, 0, 255};
    dl.add(kHudLayers - 1, HudList::Alpha, bg);
    HudTextStyle st;
    st.font = fonts.font(style_.font, style_.font_size, cv.font_height());
    if (st.font < 0) return true;
    st.align = TextAlign::Center;
    st.gradient = true;
    const float pitch = style_.spacing * sh;
    const float offset = static_cast<float>(t * std::fabs(static_cast<double>(style_.scroll_rate))) * sh;  // pixels scrolled
    const float band = std::max(1.0f, style_.margin * sh);
    for (size_t i = 0; i < lines_.size(); ++i) {
        const CreditsLine& l = lines_[i];
        if (l.kind == CreditsLine::Kind::Blank) continue;
        const float y = sh + static_cast<float>(i + 1) * pitch - offset;  // line centre, pixels from the top
        if (y < -pitch || y > sh + pitch) continue;
        // Fade in the bottom and top margin bands.
        const float edge = std::min(y, sh - y);
        const float alpha = std::clamp(edge / band, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;
        const bool header = l.kind == CreditsLine::Kind::Header;
        st.color = header ? style_.header_top : style_.top;
        st.color_bottom = header ? style_.header_bottom : style_.bottom;
        st.color.a = static_cast<uint8_t>(std::lround(alpha * 255.0f));
        st.color_bottom.a = st.color.a;
        add_text(dl, kHudLayers - 1, HudList::Text, fonts, cv, l.text, cv.pixel_to_canvas(sw * 0.5f, y), st);
        ++drawn_;
    }
    return true;
}

}  // namespace alamo::ui
