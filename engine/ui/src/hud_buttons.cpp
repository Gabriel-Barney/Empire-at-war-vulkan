#include "alamo/ui/hud_buttons.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/ui/hud_font.hpp"
#include "ui_strings.hpp"

namespace alamo::ui {

namespace {

constexpr Vec2f kFallbackButton{32, 32};

const std::string* pick(const std::vector<std::string>& v, int32_t i) {
    if (v.empty()) return nullptr;
    const auto n = static_cast<int32_t>(v.size());
    return &v[static_cast<size_t>(std::clamp(i, 0, n - 1))];
}

Vec2f offset_down(Vec2f c, Vec2f off) { return {c.x + off.x, c.y - off.y}; }  // component offsets are +Y down

Vec2f atlas_size(const TextureAtlas& atlas, const std::string* tex) {
    const AtlasEntry* e = tex && !tex->empty() ? atlas.find(*tex) : nullptr;
    return e ? Vec2f{static_cast<float>(e->width), static_cast<float>(e->height)} : Vec2f{};
}

Rgba8 with_alpha(Rgba8 c, float a) {
    c.a = static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * static_cast<float>(c.a)));
    return c;
}

Rgba8 mul(Rgba8 a, Rgba8 b) {
    auto m = [](uint8_t x, uint8_t y) { return static_cast<uint8_t>((static_cast<uint32_t>(x) * y + 127u) / 255u); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

}  // namespace

float blink_alpha(const CommandBarComponent& c, double start, double now) {
    if (start < 0.0) return 1.0f;
    const double t = now - start;
    const double duration = c.blink_duration > 0.0f ? static_cast<double>(c.blink_duration) : 3.0;
    if (t < 0.0 || (duration < 65536.0 && t > duration)) return 1.0f;
    const double half = c.blink_rate > 0.0f ? static_cast<double>(c.blink_rate) : 0.25;
    if (c.blink_fade) {
        constexpr double kPi = 3.14159265358979323846;
        return static_cast<float>(0.5 + 0.5 * std::cos(t / half * kPi));
    }
    return (static_cast<int64_t>(std::floor(t / half)) % 2) == 0 ? 1.0f : 0.0f;
}

const ButtonRuntime* HudButtons::find_state(uint32_t component) const {
    const auto it = runtime_.find(component);
    return it == runtime_.end() ? nullptr : &it->second;
}

ButtonRuntime& HudButtons::state(uint32_t component) {
    auto it = runtime_.find(component);
    if (it == runtime_.end()) {
        ButtonRuntime r;
        if (table_ && component < table_->size()) {
            r.hidden = (*table_)[component].hidden;
            r.disabled = (*table_)[component].disabled;
        }
        it = runtime_.emplace(component, std::move(r)).first;
    }
    return it->second;
}

void HudButtons::update(int32_t hover, std::span<const HudPointerEvent> events, bool shift, bool ctrl, double now,
                        std::vector<ButtonClick>& clicks) {
    auto usable = [&](int32_t c) {
        if (c < 0 || !table_ || static_cast<size_t>(c) >= table_->size()) return false;
        const ButtonRuntime* r = find_state(static_cast<uint32_t>(c));
        return !(r ? r->hidden : (*table_)[static_cast<size_t>(c)].hidden);
    };
    auto enabled = [&](int32_t c) {
        const ButtonRuntime* r = find_state(static_cast<uint32_t>(c));
        return !(r ? r->disabled : (*table_)[static_cast<size_t>(c)].disabled);
    };
    if (!usable(hover)) hover = -1;
    if (hover != hover_) {
        hover_ = hover;
        hover_since_ = now;
        if (hover >= 0 && sfx_) {
            const CommandBarComponent& c = (*table_)[static_cast<size_t>(hover)];
            if (!c.mouse_over_sfx.empty()) sfx_(c.mouse_over_sfx, hover);
        }
    }
    for (const HudPointerEvent& e : events) {
        if (e.kind == HudPointerEvent::Kind::Press && e.button == Button::Left) pressed_ = usable(e.component) ? e.component : -1;
        if (e.kind == HudPointerEvent::Kind::Release && e.button == Button::Left) pressed_ = -1;
        if (e.kind != HudPointerEvent::Kind::Click || !usable(e.component)) continue;
        if (!enabled(e.component)) continue;
        const CommandBarComponent& c = (*table_)[static_cast<size_t>(e.component)];
        if (e.button == Button::Left) {
            if (c.toggle) {
                ButtonRuntime& r = runtime_[static_cast<uint32_t>(e.component)];
                r.selected = !r.selected;
            }
            if (sfx_ && !c.click_sfx.empty()) sfx_(c.click_sfx, e.component);
        } else if (e.button == Button::Right) {
            if (sfx_ && !c.right_click_sfx.empty()) sfx_(c.right_click_sfx, e.component);
        }
        ButtonClick k;
        k.component = e.component;
        k.button = e.button;
        k.shift = shift;
        k.ctrl = ctrl;
        k.x = e.x;
        k.y = e.y;
        clicks.push_back(k);
    }
}

ButtonVisual HudButtons::visual(uint32_t component) const {
    const ButtonRuntime* r = find_state(component);
    const bool dis = r ? r->disabled : (table_ && component < table_->size() && (*table_)[component].disabled);
    if (dis) return ButtonVisual::Disabled;
    if (pressed_ == static_cast<int32_t>(component) && hover_ == static_cast<int32_t>(component)) return ButtonVisual::Pressed;
    if (r && r->selected) return ButtonVisual::Selected;
    if (hover_ == static_cast<int32_t>(component)) return ButtonVisual::MouseOver;
    return ButtonVisual::Normal;
}

namespace {
// A raw Yes / True / 1 flag (fields CommandBarComponent does not model).
bool raw_flag(const CommandBarComponent& c, std::string_view upper_tag) {
    const std::string_view v = c.raw_value(upper_tag);
    return data::parse_flag_char(v);
}
}  // namespace

Vec2f HudButtons::draw(HudDrawList& dl, const TextureAtlas& atlas, FontCache* fonts, const HudCanvas& canvas,
                       uint32_t component, Vec2f center, Vec2f size, int32_t faction_alt, double now, int32_t layer_bias) const {
    if (!table_ || component >= table_->size()) return {};
    const CommandBarComponent& c = (*table_)[component];
    ButtonRuntime def;
    def.hidden = c.hidden;
    def.disabled = c.disabled;
    const ButtonRuntime* rp = find_state(component);
    const ButtonRuntime& r = rp ? *rp : def;
    if (r.hidden) return {};
    const uint32_t layer = static_cast<uint32_t>(std::clamp(c.base_layer + layer_bias, 0, static_cast<int32_t>(kHudLayers) - 1));
    const int32_t alt = r.alt >= 0 ? r.alt : faction_alt;
    const ButtonVisual vis = visual(component);
    const float blink = blink_alpha(c, r.blink_start, now);
    const Rgba8 base = mul(c.has_color ? c.color : Rgba8{}, with_alpha(r.tint, blink));

    const std::string* blank = pick(c.blank_texture, alt);
    std::string icon_name = r.icon_override;
    if (icon_name.empty()) {
        if (const std::string* i = pick(c.icon_texture, 0)) icon_name = *i;
        else if (const std::string* ia = pick(c.icon_alternate_texture, alt)) icon_name = *ia;
    }
    Vec2f box = size;
    if (box.x <= 0 || box.y <= 0) box = atlas_size(atlas, blank);
    if (box.x <= 0 || box.y <= 0) box = atlas_size(atlas, icon_name.empty() ? nullptr : &icon_name);
    if (box.x <= 0 || box.y <= 0) box = kFallbackButton;
    box = {box.x * (size.x > 0 ? 1.0f : c.scale), box.y * (size.y > 0 ? 1.0f : c.scale)};

    // Lower effect.
    if (r.lower_effect)
        if (const std::string* le = pick(c.lower_effect_texture, 0))
            add_atlas_quad(dl, layer, c.lower_effect_additive ? HudList::Additive : HudList::Alpha, atlas, *le,
                           offset_down(center, c.lower_effect_offset), {}, c.scale, base);
    // Background.
    if (blank) add_atlas_quad(dl, layer, HudList::Alpha, atlas, *blank, center, {}, c.scale, base);
    // State texture: replaces the icon when it is a full-art variant (same atlas size), else a frame on top.
    const std::vector<std::string>* state_list = nullptr;
    Vec2f state_off{};
    switch (vis) {
        case ButtonVisual::Disabled:
            state_list = &c.disabled_texture;
            state_off = c.disabled_offset;
            break;
        case ButtonVisual::Pressed:
        case ButtonVisual::Selected: state_list = &c.selected_texture; break;
        case ButtonVisual::MouseOver:
            state_list = &c.mouse_over_texture;
            state_off = c.mouse_over_offset;
            break;
        case ButtonVisual::Normal: break;
    }
    const std::string* state_tex = state_list ? pick(*state_list, alt) : nullptr;
    const Vec2f icon_sz = atlas_size(atlas, icon_name.empty() ? nullptr : &icon_name);
    const Vec2f state_sz = atlas_size(atlas, state_tex);
    // [port] i_button_unit_frame_select* (r_NN, q1_NN) has no alpha in either atlas: black with cyan corners, opaque in the
    // alpha list. It is a highlight frame, so additive drops the black; and a unit's own icon of the same size stays under
    // it (48 x 48 hero portraits). The full-art no-alpha variants above only ever sit on the data icon, not an override.
    const AtlasEntry* state_entry = state_tex ? atlas.find(*state_tex) : nullptr;
    const bool state_opaque = state_entry && !state_entry->alpha;
    const bool replaces = state_tex && icon_sz.x > 0 && state_sz.x == icon_sz.x && state_sz.y == icon_sz.y &&
                          !(state_opaque && !r.icon_override.empty());
    Rgba8 icon_col = base;
    if (vis == ButtonVisual::Disabled && c.disabled_darken) icon_col = mul(icon_col, Rgba8{128, 128, 128, 255});
    const Vec2f icon_at = offset_down(center, c.icon_offset);
    // Can_Animate: the current frame replaces the icon, or goes over it (Animate_Upper_Effect).
    const int32_t af = c.can_animate && r.anim_start >= 0.0
                           ? anim_frame(c, r.anim_start, now, c.icon_alternate_texture.size(), r.anim_loop || c.loop_anim)
                           : -1;
    const std::string* frame_tex = af >= 0 ? &c.icon_alternate_texture[static_cast<size_t>(af)] : nullptr;
    const bool upper_frame = frame_tex && raw_flag(c, "ANIMATE_UPPER_EFFECT");
    if (frame_tex && !upper_frame && r.icon_override.empty()) icon_name = *frame_tex;
    if (!icon_name.empty() && !replaces) add_atlas_quad(dl, layer, HudList::Alpha, atlas, icon_name, icon_at, {}, c.scale, icon_col);
    if (frame_tex && upper_frame) add_atlas_quad(dl, layer, HudList::Alpha, atlas, *frame_tex, icon_at, {}, c.scale, base);
    if (state_tex)
        add_atlas_quad(dl, layer, state_opaque && !replaces ? HudList::Additive : HudList::Alpha, atlas, *state_tex,
                       replaces ? icon_at : offset_down(center, state_off), {}, c.scale, base);
    // Flash.
    if (r.flash)
        if (const std::string* fl = pick(c.flash_texture, alt))
            if ((static_cast<int64_t>(std::floor(now / 0.25)) % 2) == 0)
                add_atlas_quad(dl, layer, HudList::Alpha, atlas, *fl, center, {}, c.scale, base);
    // Build / recharge dials (radial wipe lists). cu9h t03 (CU9H_T02_BOMBING_HUD_RUN item 1b): a dial at value v leaves the
    // first v % clockwise from 12 o'clock uncovered and draws the texture over the rest, so the wipe gets 100 - v (v = 100:
    // nothing drawn).
    if (const std::string* bt = pick(c.build_texture, 0)) {
        if (r.build_fill >= 0.0f)
            add_atlas_quad(dl, layer, HudList::Alpha, atlas, *bt, offset_down(center, c.build_dial_offset), {}, c.scale, base, {},
                           100.0f - std::clamp(r.build_fill, 0.0f, 100.0f));
        if (r.build_fill2 >= 0.0f)
            add_atlas_quad(dl, layer, HudList::Additive, atlas, *bt, offset_down(center, c.build_dial2_offset), {}, c.scale, base, {},
                           100.0f - std::clamp(r.build_fill2, 0.0f, 100.0f));
    }
    if (r.overlay)
        if (const std::string* ov = pick(c.overlay_texture, 0))
            add_atlas_quad(dl, layer, HudList::Alpha, atlas, *ov, offset_down(center, c.overlay_offset), {}, c.scale, base);
    if (r.overlay2)
        if (const std::string* ov = pick(c.overlay2_texture, 0))
            add_atlas_quad(dl, layer, HudList::Alpha, atlas, *ov, offset_down(center, c.overlay2_offset), {}, c.scale, base);
    if (r.upper_effect)
        if (const std::string* ue = pick(c.upper_effect_texture, 0))
            add_atlas_quad(dl, layer, HudList::Alpha, atlas, *ue, offset_down(center, c.upper_effect_offset), {}, c.scale, base);
    // Text.
    if (fonts && (!r.text.empty() || !r.text2.empty())) {
        HudTextStyle st;
        st.font = fonts->font(c.font_name.empty() ? std::string(kFallbackFontFace) : c.font_name,
                              c.font_point_size > 0 ? c.font_point_size : 7.0f, canvas.font_height());
        st.color = with_alpha(r.text_color_set ? r.text_color : c.has_text_color ? c.text_color : Rgba8{}, blink);
        st.outline = c.text_outline;
        st.emboss = c.text_emboss;
        st.align = c.left_justified ? TextAlign::Left : c.right_justified ? TextAlign::Right : TextAlign::Center;
        if (!r.text.empty()) {
            const Vec2f at = offset_down(center, c.text_offset);
            if (c.max_text_width > 0.0f)
                add_text_block(dl, layer, HudList::Text, *fonts, canvas, r.text, at, c.max_text_width, st);
            else
                add_text(dl, layer, HudList::Text, *fonts, canvas, r.text, at, st);
        }
        if (!r.text2.empty()) {
            HudTextStyle st2 = st;
            if (c.has_text_color2) st2.color = with_alpha(c.text_color2, blink);
            add_text(dl, layer, HudList::Text, *fonts, canvas, r.text2, offset_down(center, c.text_offset2), st2);
        }
    }
    return box;
}

int32_t anim_frame(const CommandBarComponent& c, double start, double now, size_t frames, bool loop) {
    if (frames == 0 || start < 0.0 || now < start) return frames && start >= 0.0 ? 0 : -1;
    const double fps = c.anim_fps > 0.0f ? static_cast<double>(c.anim_fps) : 10.0;
    const auto k = static_cast<int64_t>(std::floor((now - start) * fps));
    const auto n = static_cast<int64_t>(frames);
    const bool ping = n > 1 && raw_flag(c, "ANIMATE_BACK");
    const int64_t period = ping ? 2 * n - 2 : n;
    if (!loop && k >= period) return -1;
    const int64_t i = k % period;
    return static_cast<int32_t>(ping && i >= n ? period - i : i);
}

void HudButtons::play_anim(uint32_t component, double now, bool loop, std::string name) {
    ButtonRuntime& r = state(component);
    r.anim = std::move(name);
    r.anim_loop = loop;
    r.anim_start = now;
}

void HudButtons::stop_anim(uint32_t component) {
    ButtonRuntime& r = state(component);
    r.anim.clear();
    r.anim_loop = false;
    r.anim_start = -1.0;
}

TooltipConfig tooltip_config_from(const data::XmlNode& root) {
    TooltipConfig cfg;
    auto ms = [&](std::string_view tag, double& out) {
        const std::string_view t = root.child_text(tag);
        if (!t.empty()) out = static_cast<double>(data::parse_float_or(t, static_cast<float>(out * 1000.0))) / 1000.0;
    };
    ms("Tooltip_Delay", cfg.delay);
    ms("Encyclopedia_Delay", cfg.encyclopedia_delay);
    ms("Long_Encyclopedia_Delay", cfg.long_encyclopedia_delay);
    if (const std::string_view t = root.child_text("Encyclopedia_Fade_Rate"); !t.empty())
        cfg.encyclopedia_fade_rate = data::parse_float_or(t, cfg.encyclopedia_fade_rate);
    if (const std::string_view t = root.child_text("Encyclopedia_Min_Display_Time"); !t.empty())
        cfg.encyclopedia_min_display =
            static_cast<double>(data::parse_float_or(t, static_cast<float>(cfg.encyclopedia_min_display)));
    return cfg;
}

void TooltipState::update(const HudButtons& buttons, const ComponentTable& table, const TooltipConfig& cfg, double now) {
    component_ = -1;
    const int32_t h = buttons.hover();
    if (h < 0 || static_cast<size_t>(h) >= table.size()) return;
    const ButtonRuntime* r = buttons.find_state(static_cast<uint32_t>(h));
    if (table[static_cast<size_t>(h)].tooltip_text.empty() && (!r || (r->tooltip_key.empty() && r->tooltip_text.empty()))) return;
    if (buttons.pressed() >= 0) return;  // no tooltip while a button is held
    if (now - buttons.hover_since() + 1e-9 < cfg.delay) return;
    component_ = h;
}

std::string TooltipState::key(const HudButtons& buttons, const ComponentTable& table) const {
    if (component_ < 0 || static_cast<size_t>(component_) >= table.size()) return {};
    const CommandBarComponent& c = table[static_cast<size_t>(component_)];
    const ButtonRuntime* r = buttons.find_state(static_cast<uint32_t>(component_));
    const int32_t i = r ? r->tooltip_index : 0;
    if (i < 0) return {};
    if (r && !r->tooltip_key.empty()) return r->tooltip_key;  // ui3 t07: ability / unit text
    if (c.tooltip_text.empty()) return {};
    return *pick(c.tooltip_text, i);
}

bool TooltipState::draw(HudDrawList& dl, const TextureAtlas& atlas, FontCache& fonts, const HudCanvas& canvas,
                        const ComponentTable& table, const HudButtons& buttons, const TextDatabase* text, Vec2f cursor_px,
                        int32_t faction_alt, const TooltipConfig& cfg) const {
    const std::string k = key(buttons, table);
    const ButtonRuntime* rt = component_ >= 0 ? buttons.find_state(static_cast<uint32_t>(component_)) : nullptr;
    const std::u32string literal = rt ? rt->tooltip_text : std::u32string();  // [port] a player-given name
    if (k.empty() && literal.empty()) return false;
    // ui3 t07: a runtime tooltip_key may list several keys separated by spaces (ability name + description): one line each.
    std::u32string body;
    size_t at = 0;
    while (at < k.size()) {
        size_t e = k.find(' ', at);
        if (e == std::string::npos) e = k.size();
        if (e > at) {
            const std::string one = k.substr(at, e - at);
            if (!body.empty()) body += U'\n';
            body += text ? utf16_to_utf32(text->text(one)) : ascii32(one);
        }
        at = e + 1;
    }
    if (!literal.empty()) {
        if (!body.empty()) body += U'\n';
        body += literal;
    }
    return draw_text(dl, atlas, fonts, canvas, table, body, cursor_px, faction_alt, cfg);
}

bool TooltipState::draw_text(HudDrawList& dl, const TextureAtlas& atlas, FontCache& fonts, const HudCanvas& canvas,
                             const ComponentTable& table, const std::u32string& body, Vec2f cursor_px,
                             int32_t faction_alt, const TooltipConfig& cfg) {
    if (body.empty()) return false;
    const CommandBarComponent* back = table.get("tooltip_back");
    const CommandBarComponent* font_c = table.get("tooltip_left_text");
    const uint32_t layer = back ? static_cast<uint32_t>(back->base_layer) : 10u;
    HudTextStyle st;
    st.font = fonts.font(font_c && !font_c->font_name.empty() ? font_c->font_name : std::string("Arial"),
                         font_c && font_c->font_point_size > 0 ? font_c->font_point_size : 7.0f, canvas.font_height());
    // [port] the tooltip text uses the frame's light colour: tooltip_left_text's Color is a dark blue meant for the
    // encyclopedia panel; white keeps it readable on i_tooltip_back.
    st.color = Rgba8{230, 230, 230, 255};
    st.align = TextAlign::Left;
    const float wrap = font_c && font_c->max_text_width > 0 ? font_c->max_text_width : cfg.default_wrap_units;
    const TextBlock tb = measure_text_block(fonts, canvas, body, wrap, st);
    const float pad = 6.0f;
    const Vec2f size{tb.width + 2 * pad, tb.height + 2 * pad};
    // HUD_GAPS §2.1: below-right of the cursor (left edge cursor + 8 px, top edge cursor - 24 px); flips left of the
    // cursor (by width + 16 px) at the right edge and above it (by height + 48 px) at the bottom; then kept inside.
    const Vec2f cur = canvas.pixel_to_canvas(cursor_px.x, cursor_px.y);
    const Vec2f d = canvas.pixel_to_canvas(cursor_px.x + 8.0f, cursor_px.y + 24.0f);
    const float ox = std::fabs(d.x - cur.x), oy = std::fabs(d.y - cur.y);  // 8 px, 24 px in canvas units
    const float hw = 0.5f * canvas.width, hh = 0.5f * canvas.height;
    Vec2f min{cur.x + ox, cur.y - oy - size.y};
    if (min.x + size.x > hw) min.x -= size.x + 2.0f * ox;
    if (min.y < -hh) min.y += size.y + 2.0f * oy;
    min.x = std::clamp(min.x, -hw + kTooltipFrameMargin, hw - size.x - kTooltipFrameMargin);  // the rim stays on screen too
    min.y = std::clamp(min.y, -hh + kTooltipFrameMargin, hh - size.y - kTooltipFrameMargin);
    const Vec2f center{min.x + 0.5f * size.x, min.y + 0.5f * size.y};
    add_tooltip_box(dl, layer, atlas, back, faction_alt, center, size, Rgba8{});
    add_text_block(dl, layer, HudList::Text, fonts, canvas, body, {min.x + pad, min.y + size.y - pad}, wrap, st);
    return true;
}

void add_tooltip_box(HudDrawList& dl, uint32_t layer, const TextureAtlas& atlas, const CommandBarComponent* back,
                     int32_t faction_alt, Vec2f center, Vec2f size, Rgba8 back_color) {
    if (const std::string* fr = back ? pick(back->icon_alternate_texture, faction_alt) : nullptr)
        add_atlas_quad(dl, layer, HudList::Alpha, atlas, *fr, center,
                       {size.x + 2.0f * kTooltipFrameMargin, size.y + 2.0f * kTooltipFrameMargin}, 1.0f, Rgba8{});
    const std::string* bt = back ? pick(back->blank_texture, 0) : nullptr;
    if (!bt || !add_atlas_quad(dl, layer, HudList::Alpha, atlas, *bt, center, size, 1.0f, back_color))
        add_atlas_quad(dl, layer, HudList::Alpha, atlas, {}, center, {}, 1.0f, Rgba8{22, 33, 73, back_color.a}, size);
}

std::map<std::string, std::string> parse_dialog_tooltips(const data::XmlNode& root) {
    std::map<std::string, std::string> out;
    const data::XmlNode tips = root.child("Tooltips");
    for (data::XmlNode t : tips.children()) {
        const std::string_view id = t.child_text("TextID");
        if (!id.empty()) out[std::string(t.name())] = std::string(id);
    }
    return out;
}

}  // namespace alamo::ui
