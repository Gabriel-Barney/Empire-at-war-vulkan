#include "alamo/ui/hud_panels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "alamo/core/hash.hpp"
#include "alamo/core/text.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

std::u32string ascii32(std::string_view s) {
    std::u32string o;
    for (char c : s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    return o;
}
std::u32string num32(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    return ascii32(b);
}
std::string fmt(const char* f, int a) {
    char b[48];
    std::snprintf(b, sizeof b, f, a);
    return b;
}
std::string fmt2(const char* f, int a, int b2) {
    char b[48];
    std::snprintf(b, sizeof b, f, a, b2);
    return b;
}
std::string fmts(const char* f, const char* s, int a) {
    char b[48];
    std::snprintf(b, sizeof b, f, s, a);
    return b;
}
const std::string* pick(const std::vector<std::string>& v, int32_t i) {
    if (v.empty()) return nullptr;
    const size_t k = i < 0 ? 0 : std::min(static_cast<size_t>(i), v.size() - 1);
    return &v[k];
}
constexpr int32_t kShellLayerMin = 2;  // as the tactical / galactic HUDs: shell-bone components above the first shell half
uint32_t layer_of(const CommandBarComponent& c, const ComponentPlacement& p) {
    int32_t l = c.base_layer;
    if (p.from_bone) l = std::max(l, kShellLayerMin);
    return static_cast<uint32_t>(std::clamp(l, 0, static_cast<int32_t>(kHudLayers) - 1));
}
bool has_art(const CommandBarComponent& c) {
    return !c.blank_texture.empty() || !c.icon_texture.empty() || !c.icon_alternate_texture.empty() ||
           !c.mouse_over_texture.empty() || !c.selected_texture.empty();
}

// Slot roles (PanelSlot::value) shared by the panels.
constexpr int32_t kRoleText = -1, kRoleClose = -2, kRoleBar = -3, kRoleDeco = -4;

HudTextStyle style_of(const CommandBarComponent* c, FontCache& fonts, const HudCanvas& cv, float default_pt,
                      Rgba8 default_color, TextAlign default_align) {
    HudTextStyle st;
    const std::string face = c && !c->font_name.empty() ? c->font_name : std::string("Arial");
    st.font = fonts.font(face, c && c->font_point_size > 0 ? c->font_point_size : default_pt, cv.screen_height);
    st.color = c && c->has_text_color ? c->text_color : (c && c->has_color ? c->color : default_color);
    st.outline = c && c->text_outline;
    st.emboss = c && c->text_emboss;
    st.align = c ? (c->left_justified ? TextAlign::Left : c->right_justified ? TextAlign::Right : default_align) : default_align;
    return st;
}

// t40: the pop-up's layout, canvas units from the panel's top-left corner. GameConstants Encyclopedia_Population_Offset 11,
// _Name_Offset 68, _Cost_Offset 258, _Icon_X/Y_Offset 39 / -12, _Class_Y_Offset 5 (all confirmed on the captures); the rest
// measured on C3_tooltip_slot_h1_full.png [inferred].
constexpr float kEncPopulationX = 11.0f, kEncNameX = 68.0f, kEncCostX = 258.0f, kEncIconX = 39.0f, kEncIconDown = 12.0f,
                kEncClassDown = 5.0f;
constexpr float kEncBandH = 20.0f;         // e_topbar / e_topbar2
constexpr float kEncAbilityBase = 32.0f;   // encyclopedia_icon Size.y (0.66) scales this: 21 units on the captures
constexpr float kEncAbilityRight = 260.0f, kEncAbilityStep = 25.0f, kEncAbilityDown = 3.5f;  // below the band
constexpr size_t kEncMaxAbilities = 5;     // [port] the class text keeps a column
constexpr float kEncHeadH = 47.0f, kEncLineX = 5.0f, kEncLineW = 232.0f, kEncLineH = 27.0f, kEncLine2H = 13.0f, kEncLine2Up = 2.5f;
constexpr float kEncDescGap = 4.0f, kEncAgainstGap = 14.0f, kEncAgainstBottom = 6.0f, kEncFrameW = 129.0f, kEncFrameH = 43.0f,
                kEncCellW = 43.0f;
constexpr size_t kEncCells = 3;
constexpr Rgba8 kEncStrong{0, 192, 0, 255}, kEncWeak{192, 0, 0, 255}, kEncPopulation{255, 215, 56, 255};

}  // namespace

// ==== Encyclopedia =================================================================================================
void EncyclopediaPanel::init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text) {
    table_ = &table;
    atlas_ = &atlas;
    fonts_ = &fonts;
    text_ = text;
}

void EncyclopediaPanel::update(int32_t component, uint32_t object, double now) {
    const double prev = has_last_ ? std::min(last_, now) : now;
    has_last_ = true;
    last_ = now;
    if (component != component_ || object != object_) {
        component_ = component;
        object_ = object;
        since_ = now;
    }
    const double wait = cfg_.encyclopedia_wait();
    const bool target = component_ >= 0 || object_ != 0;
    const bool ready = target && now - since_ + 1e-9 >= wait;
    const bool same = stage_ == Stage::Shown && shown_component_ == component_ && shown_object_ == object_;
    if (ready && !same) {
        // The countdown expired: (re)build the pop-up for this subject. It appeared at the expiry instant.
        shown_component_ = component_;
        shown_object_ = object_;
        shown_at_ = std::min(now, since_ + wait);
        alpha_ = 0.0f;
        stage_ = Stage::Shown;
    }
    if (stage_ == Stage::Hidden) return;
    const double rate = static_cast<double>(std::max(cfg_.encyclopedia_fade_rate, 0.0f));
    double a = static_cast<double>(alpha_);
    const double from = std::max(prev, shown_at_);
    if (ready && shown_component_ == component_ && shown_object_ == object_) {
        a += rate * std::max(0.0, now - from);
    } else {
        // Lost: keeps fading in until the minimum display time is up, then fades out.
        const double hold = shown_at_ + cfg_.encyclopedia_min_display;
        a += rate * std::max(0.0, std::min(now, hold) - from);
        a = std::min(a, 1.0);
        a -= rate * std::max(0.0, now - std::max(from, hold));
        if (rate <= 0.0 && now >= hold) a = 0.0;
    }
    alpha_ = static_cast<float>(std::clamp(a, 0.0, 1.0));
    if (alpha_ <= 0.0f && !(ready && shown_component_ == component_ && shown_object_ == object_) &&
        now >= shown_at_ + cfg_.encyclopedia_min_display) {
        stage_ = Stage::Hidden;  // faded out: destroyed
        shown_component_ = -1;
        shown_object_ = 0;
    }
}

Vec2f EncyclopediaPanel::hint_anchor(const HudLayout& layout, const HudCanvas& cv, bool zoomed) const {
    Vec2f at{-0.5f * cv.width + 65.0f, -0.5f * cv.height + 309.0f};  // [port] fallback
    bool found = false;
    const std::vector<uint32_t> shells = layout.shells();
    for (const char* name : kHintBones) {
        for (uint32_t sh : shells) {
            if (!layout.shell_active(sh)) continue;
            const Vec2f off = layout.shell_offset(sh);
            for (const ShellBone& b : layout.shell_bones(sh))
                if (ascii_iequals(b.name, name)) {
                    at = {b.x + off.x, b.y + off.y};
                    found = true;
                    break;
                }
            if (found) break;
        }
        if (found) break;
    }
    const CommandBarComponent* back = table_ ? table_->get(zoomed ? "zoomed_back" : "encyclopedia_back") : nullptr;
    if (!back && table_) back = table_->get("encyclopedia_back");
    if (back) at = {at.x + back->default_offset.x, at.y + back->default_offset.y};
    return at;
}

Vec2f EncyclopediaPanel::build(HudDrawList& dl, const HudCanvas& cv, const EncyclopediaEntry& e, Vec2f cursor_px,
                               std::optional<Vec2f> anchor, int32_t faction_alt, bool zoomed) const {
    return build_at(dl, cv, e, cursor_px, anchor, std::nullopt, faction_alt, zoomed);
}

Vec2f EncyclopediaPanel::build(HudDrawList& dl, const HudCanvas& cv, const EncyclopediaEntry& e, const HudLayout& layout,
                               int32_t faction_alt, bool zoomed) const {
    return build_at(dl, cv, e, {}, std::nullopt, hint_anchor(layout, cv, zoomed), faction_alt, zoomed);
}

Vec2f EncyclopediaPanel::build_at(HudDrawList& out, const HudCanvas& cv, const EncyclopediaEntry& e, Vec2f cursor_px,
                                  std::optional<Vec2f> anchor, std::optional<Vec2f> bottom_left, int32_t faction_alt,
                                  bool zoomed) const {
    if (stage_ == Stage::Hidden || !table_ || !fonts_) return {};
    const std::string pre = zoomed ? "zoomed_" : "encyclopedia_";
    const CommandBarComponent* back = table_->get(pre + "back");
    const CommandBarComponent* hdr = table_->get(pre + "header_text");
    const CommandBarComponent* txt = table_->get(pre + "text");
    const CommandBarComponent* rgt = table_->get(pre + "right_text");
    const CommandBarComponent* ctr = table_->get(pre + "center_text");
    const CommandBarComponent* cst = table_->get(pre + "cost_text");
    const CommandBarComponent* ico = table_->get("encyclopedia_icon");
    if (!hdr) hdr = table_->get("encyclopedia_header_text");
    if (!cst) cst = table_->get("encyclopedia_cost_text");
    const uint32_t layer = static_cast<uint32_t>(std::clamp(back ? back->base_layer : (zoomed ? 4 : 9), 0, 12));
    const float width = back && back->has_size ? back->size.x : 262.0f;
    const float row = back && back->has_size && back->size.y > 0 ? back->size.y : 14.0f;
    const Vec2f pad = back && (back->offset.x != 0 || back->offset.y != 0) ? back->offset : Vec2f{5, 2};
    FontCache& fonts = *fonts_;
    const HudTextStyle hs = style_of(hdr, fonts, cv, 7.0f, Rgba8{}, TextAlign::Left);
    HudTextStyle ts = style_of(txt, fonts, cv, 7.0f, Rgba8{192, 192, 192, 255}, TextAlign::Left);
    ts.color.a = 255;  // [inferred] base-game captures (C3_*tooltip*): the text is solid 192 192 192 despite Text_Color alpha 200
    HudTextStyle rs = style_of(rgt, fonts, cv, 7.0f, Rgba8{192, 192, 192, 255}, TextAlign::Right);
    rs.align = TextAlign::Right;
    HudTextStyle cs = style_of(ctr, fonts, cv, 7.0f, Rgba8{}, TextAlign::Center);
    cs.align = TextAlign::Center;
    HudTextStyle ks = style_of(cst, fonts, cv, 7.0f, Rgba8{192, 192, 192, 255}, TextAlign::Right);
    ks.align = TextAlign::Right;
    // Top band: population circle, icon at the icon scale (encyclopedia_icon Size.x), name, cost.
    const AtlasEntry* ie = e.icon.empty() ? nullptr : atlas_->find(e.icon);
    const float icon_scale = ico && ico->has_size && ico->size.x > 0 ? ico->size.x : 0.75f;
    const Vec2f icon_size = ie ? Vec2f{static_cast<float>(ie->width) * icon_scale, static_cast<float>(ie->height) * icon_scale}
                               : Vec2f{};
    const bool longform = !e.description.empty();
    // Drawn into a scratch list, then copied with every alpha scaled by the fade.
    HudDrawList scratch;
    HudDrawList& dl = scratch;
    const float wrap = width - 2.0f * pad.x;
    const TextBlock db = longform ? measure_text_block(fonts, cv, e.description, wrap, ts) : TextBlock{};
    // HUD_GAPS §2.1 / QA U4: every text stays inside the box. The name wraps left of the cost; a row keeps its label on the
    // left and wraps its value in what is left of the width, right-aligned [port: the row grows by whole lines]. A lone
    // word wider than its column splits (wrap_text).
    constexpr float kColumnGap = 6.0f;  // [port] canvas units between the columns
    const float cost_w = e.cost.empty() ? 0.0f : measure_text_block(fonts, cv, e.cost, 0.0f, ks).width;
    const float cost_right = std::min(kEncCostX, width - pad.x);
    const float name_wrap = std::max((cost_w > 0.0f ? cost_right - cost_w - kColumnGap : width - pad.x) - kEncNameX, 1.0f);
    const TextBlock nb = measure_text_block(fonts, cv, e.header, name_wrap, hs);
    const float band_h = std::max(kEncBandH, nb.height + 2.0f);  // [port] a long name wraps and the band grows (QA U4)
    // Class + ability icons under the band, right of the unit icon.
    const size_t ability_n = std::min(e.abilities.size(), kEncMaxAbilities);
    const float ability_size = kEncAbilityBase * (ico && ico->has_size && ico->size.y > 0 ? ico->size.y : 0.66f);
    const float ability_left = kEncAbilityRight - static_cast<float>(ability_n) * kEncAbilityStep + (kEncAbilityStep - ability_size);
    const float class_wrap = std::max((ability_n > 0 ? ability_left - kColumnGap : width - pad.x) - kEncNameX, 1.0f);
    const TextBlock cb = measure_text_block(fonts, cv, e.unit_class, class_wrap, ts);
    const bool unit_head = ie || !e.unit_class.empty() || ability_n > 0;
    const float head_h = unit_head ? std::max({band_h + kEncHeadH - kEncBandH, band_h + kEncClassDown + cb.height + 3.0f,
                                               0.5f * band_h + kEncIconDown + 0.5f * icon_size.y + 1.0f,
                                               band_h + kEncAbilityDown + ability_size + 2.0f})
                                   : band_h;
    // Strong / Weak rows: the labels sit on their frames.
    const bool against = !e.strong.empty() || !e.weak.empty();
    const float label_wrap = kEncFrameW - 2.0f;
    const TextBlock sb = against ? measure_text_block(fonts, cv, e.strong_label, label_wrap, ts) : TextBlock{};
    const TextBlock wb = against ? measure_text_block(fonts, cv, e.weak_label, label_wrap, ts) : TextBlock{};
    const float against_h = against ? std::max({kEncAgainstGap, sb.height, wb.height}) + kEncFrameH + kEncAgainstBottom : 0.0f;
    struct RowLayout {
        float left_wrap = 0.0f, right_wrap = 0.0f, height = 0.0f, inset = 0.0f;
    };
    std::vector<RowLayout> row_layout;
    row_layout.reserve(e.rows.size());
    float rows_h = 0.0f;
    for (const EncyclopediaRow& r : e.rows) {
        RowLayout rl;
        float block_h = 0.0f, line_h = 0.0f;
        const auto add_block = [&](const std::u32string& s, float w, const HudTextStyle& st) {
            const TextBlock b = measure_text_block(fonts, cv, s, w, st);
            block_h = std::max(block_h, b.height);
            if (b.lines > 0) line_h = std::max(line_h, b.height / static_cast<float>(b.lines));
        };
        if (!r.center.empty()) {
            add_block(r.center, wrap, cs);
        } else {
            // The label takes what it needs, at most half the width (only a very long label wraps).
            const float left_w = r.left.empty() ? 0.0f : std::min(measure_text_block(fonts, cv, r.left, 0.0f, ts).width, 0.5f * wrap);
            rl.left_wrap = 0.5f * wrap;
            rl.right_wrap = wrap - (left_w > 0.0f ? left_w + kColumnGap : 0.0f);
            if (!r.left.empty()) add_block(r.left, rl.left_wrap, ts);
            if (!r.right.empty()) add_block(r.right, rl.right_wrap, rs);
        }
        rl.inset = 0.5f * std::max(row - line_h, 0.0f);  // a single line sits centred in the row
        rl.height = std::max(row, block_h + 2.0f * rl.inset);
        rows_h += rl.height;
        row_layout.push_back(rl);
    }
    const float height = head_h + rows_h + (longform ? kEncDescGap + db.height : 0.0f) + (against ? against_h : 2.0f * pad.y);
    // Placement: top-left corner.
    Vec2f tl;
    if (bottom_left) {
        tl = {bottom_left->x, bottom_left->y + height};
    } else if (anchor) {
        tl = *anchor;
    } else {
        const Vec2f cur = cv.pixel_to_canvas(cursor_px.x, cursor_px.y);
        const Vec2f off = zoomed && back ? back->default_offset : Vec2f{16.0f, 16.0f};
        tl = {cur.x + off.x, cur.y + off.y + height};
    }
    const float hw = 0.5f * cv.width, hh = 0.5f * cv.height;
    tl.x = std::clamp(tl.x, -hw + kTooltipFrameMargin, hw - width - kTooltipFrameMargin);  // the rim stays on screen too
    tl.y = std::clamp(tl.y, -hh + height + kTooltipFrameMargin, hh - kTooltipFrameMargin);
    const Vec2f center{tl.x + 0.5f * width, tl.y - 0.5f * height};
    // Background + faction frame.
    // [inferred] base-game captures (C3_*tooltip*): the inside is e_background at full opacity (22 33 73), nothing shows
    // through, whatever encyclopedia_back's Color alpha 128 / Lower_Effect_Additive are for (no lower effect texture here).
    Rgba8 bc = back && back->has_color ? back->color : Rgba8{};
    bc.a = 255;
    add_tooltip_box(dl, layer, *atlas_, back, faction_alt, center, {width, height}, bc);
    // Top band, the line under the head, the unit icon over them. The band and the frames below are opaque e_* art.
    float y = tl.y - 0.5f * band_h;
    add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, e.population > 0 ? "e_topbar.tga" : "e_topbar2.tga", {center.x, y}, {width, band_h}, 1.0f, Rgba8{});
    if (unit_head)
        add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, "e_line.tga",
                       {tl.x + kEncLineX + 0.5f * kEncLineW, tl.y - head_h + 0.5f * kEncLineH}, {kEncLineW, kEncLineH}, 1.0f, Rgba8{});
    if (ie) add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, e.icon, {tl.x + kEncIconX, y - kEncIconDown}, icon_size, 1.0f, Rgba8{});
    if (e.population > 0) {
        HudTextStyle ps = style_of(ico, fonts, cv, 6.0f, kEncPopulation, TextAlign::Center);
        ps.color = kEncPopulation;  // [inferred] capture h1_full
        ps.align = TextAlign::Center;
        add_text(dl, layer, HudList::Text, fonts, cv, utf8_to_utf32(std::to_string(e.population)), {tl.x + kEncPopulationX, y}, ps);
    }
    add_text_block(dl, layer, HudList::Text, fonts, cv, e.header, {tl.x + kEncNameX, y + 0.5f * nb.height}, name_wrap, hs);
    if (!e.cost.empty()) add_text(dl, layer, HudList::Text, fonts, cv, e.cost, {tl.x + cost_right, y}, ks);
    if (!e.unit_class.empty())
        add_text_block(dl, layer, HudList::Text, fonts, cv, e.unit_class, {tl.x + kEncNameX, tl.y - band_h - kEncClassDown}, class_wrap, ts);
    for (size_t i = 0; i < ability_n; ++i)
        add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, e.abilities[i],
                       {tl.x + ability_left + static_cast<float>(i) * kEncAbilityStep + 0.5f * ability_size,
                        tl.y - band_h - kEncAbilityDown - 0.5f * ability_size},
                       {ability_size, ability_size}, 1.0f, Rgba8{});
    y = tl.y - head_h;
    // Rows.
    for (size_t ri = 0; ri < e.rows.size(); ++ri) {
        const EncyclopediaRow& r = e.rows[ri];
        const RowLayout& rl = row_layout[ri];
        const float top = y - rl.inset;
        if (!r.center.empty()) {
            add_text_block(dl, layer, HudList::Text, fonts, cv, r.center, {center.x, top}, wrap, cs);
        } else {
            if (!r.left.empty()) add_text_block(dl, layer, HudList::Text, fonts, cv, r.left, {tl.x + pad.x, top}, rl.left_wrap, ts);
            if (!r.right.empty())
                add_text_block(dl, layer, HudList::Text, fonts, cv, r.right, {tl.x + width - pad.x, top}, rl.right_wrap, rs);
        }
        y -= rl.height;
    }
    if (longform) {
        y -= kEncDescGap;
        add_text_block(dl, layer, HudList::Text, fonts, cv, e.description, {tl.x + pad.x, y}, wrap, ts);
        y -= db.height;
    }
    if (against) {
        add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, "e_line2.tga",
                       {tl.x + kEncLineX + 0.5f * kEncLineW, y + kEncLine2Up - 0.5f * kEncLine2H}, {kEncLineW, kEncLine2H}, 1.0f, Rgba8{});
        const float frame_top = y - std::max({kEncAgainstGap, sb.height, wb.height});
        const auto side = [&](float left, const std::vector<std::string>& icons, const std::u32string& label, const TextBlock& lb, Rgba8 tint) {
            HudTextStyle ls = ts;
            ls.color = tint;  // [inferred] the captures' colours: e_against_frame is grey, tinted like its label
            add_text_block(dl, layer, HudList::Text, fonts, cv, label, {left + 1.0f, frame_top + lb.height}, label_wrap, ls);
            add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, "e_against_frame.tga", {left + 0.5f * kEncFrameW, frame_top - 0.5f * kEncFrameH},
                           {kEncFrameW, kEncFrameH}, 1.0f, tint);
            for (size_t i = 0; i < icons.size() && i < kEncCells; ++i)  // [inferred] the frame has three cells
                add_atlas_quad(dl, layer, HudList::Alpha, *atlas_, icons[i],
                               {left + (static_cast<float>(i) + 0.5f) * kEncCellW, frame_top - 0.5f * kEncFrameH}, {kEncCellW, kEncCellW}, 1.0f, Rgba8{});
        };
        side(tl.x, e.strong, e.strong_label, sb, kEncStrong);
        side(tl.x + width - kEncFrameW, e.weak, e.weak_label, wb, kEncWeak);
    }
    const float fade = std::clamp(alpha_, 0.0f, 1.0f);
    auto scale = [fade](Rgba8 c) {
        c.a = static_cast<uint8_t>(std::lround(static_cast<float>(c.a) * fade));
        return c;
    };
    for (uint32_t l = 0; l < kHudLayers; ++l)
        for (uint32_t li = 0; li < kHudSubLists; ++li)
            for (HudQuad q : scratch.items(l, static_cast<HudList>(li))) {
                q.color = scale(q.color);
                q.color_bottom = scale(q.color_bottom);
                out.add(l, static_cast<HudList>(li), q);
            }
    return {width, height};
}

// ==== HudPanel =====================================================================================================
bool HudPanel::init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text) {
    table_ = &table;
    atlas_ = &atlas;
    fonts_ = &fonts;
    text_ = text;
    buttons_.bind(table);
    shells_.clear();
    bool any = false;
    for (const std::string& n : shell_names()) {
        shells_.push_back(table.find(n));
        any = any || shells_.back() >= 0;
    }
    resize(canvas_.screen_width, canvas_.screen_height);
    return any;
}

void HudPanel::resize(uint32_t width, uint32_t height) {
    const HudCanvas next = HudCanvas::make(width, height);
    if (fonts_ && next.screen_height != canvas_.screen_height) fonts_->clear();
    canvas_ = next;
    if (table_) {
        fallback_layout();
        layout_.update(canvas_, *table_);
    }
}

void HudPanel::put(std::string_view name, float x, float y) {
    const int32_t i = find(name);
    if (i >= 0) layout_.set_fallback(static_cast<uint32_t>(i), {x, y});
}

int32_t HudPanel::hit_test(int32_t px, int32_t py) const {
    if (!open_) return -1;
    const Vec2f c = canvas_.pixel_to_canvas(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f);
    int32_t best = -1;
    uint32_t best_layer = 0;
    for (const HudHitBox& h : hits_) {
        if (c.x < h.min.x || c.x > h.max.x || c.y < h.min.y || c.y > h.max.y) continue;
        if (best < 0 || h.layer >= best_layer) {
            best = h.component;
            best_layer = h.layer;
        }
    }
    return best;
}

const PanelSlot* HudPanel::slot_of(int32_t component) const {
    if (component < 0) return nullptr;
    for (const PanelSlot& s : slots_)
        if (s.component == component) return &s;
    return nullptr;
}

bool HudPanel::owns(int32_t component) const { return open_ && slot_of(component) != nullptr; }

void HudPanel::begin(const PanelFrame& f, int32_t shell) {
    dl_.clear();
    hits_.clear();
    slots_.clear();
    open_ = f.open && table_ != nullptr;
    active_shell_ = shell >= 0 && static_cast<size_t>(shell) < shells_.size() ? shells_[static_cast<size_t>(shell)] : -1;
    if (!table_) return;
    for (size_t k = 0; k < shells_.size(); ++k)
        if (shells_[k] >= 0) layout_.set_shell_active(static_cast<uint32_t>(shells_[k]), open_ && static_cast<int32_t>(k) == shell);
    layout_.update(canvas_, *table_);
}

void HudPanel::draw_slots(const PanelFrame& f) {
    if (!open_) return;
    for (const PanelSlot& s : slots_) {
        if (s.component < 0) continue;
        const auto u = static_cast<uint32_t>(s.component);
        const CommandBarComponent& c = (*table_)[u];
        const ComponentPlacement& p = layout_.placement(u);
        ButtonRuntime& r = buttons_.state(u);
        r.hidden = false;
        r.disabled = s.disabled;
        r.selected = s.selected;
        r.flash = s.flash;
        r.icon_override = s.icon;
        r.text = s.text;
        r.text2 = s.text2;
        r.alt = s.alt;
        r.build_fill = s.dial >= 0.0f ? std::clamp(s.dial, 0.0f, 1.0f) * 100.0f : -1.0f;
        if (!p.placed) continue;
        const uint32_t layer = layer_of(c, p);
        if (c.type == ComponentType::Bar) {
            if (s.bar < 0.0f) continue;
            const float v = std::clamp(s.bar, 0.0f, 1.0f);
            const Vec2f fb = p.size.x > 0 ? Vec2f{p.size.x - 2.0f, std::max(2.0f, p.size.y - 4.0f)} : Vec2f{40.0f, 4.0f};
            const Vec2f at = p.size.x > 0 ? p.rect_center : p.center;
            const int32_t lvl = static_cast<int32_t>(std::lround(v * static_cast<float>(std::max(1, c.max_bar_level))));
            if (const std::string* frame = pick(c.bar_texture, lvl))
                add_atlas_quad(dl_, layer, HudList::Alpha, *atlas_, *frame, at, {}, c.scale, Rgba8{});
            const std::string* ov = c.smooth_bar ? (c.bar_overlay.empty() ? nullptr : &c.bar_overlay.back()) : pick(c.bar_overlay, lvl);
            if (!ov || !add_atlas_quad_clipped(dl_, layer, HudList::Alpha, *atlas_, *ov, at, {}, c.scale, c.smooth_bar ? v : 1.0f, Rgba8{}))
                add_atlas_quad_clipped(dl_, layer, HudList::Alpha, *atlas_, {}, at, {}, 1.0f, v, Rgba8{40, 220, 40, 255}, fb);
            continue;
        }
        Vec2f sz = buttons_.draw(dl_, *atlas_, fonts_, canvas_, u, p.center, p.size, f.faction_alt, f.time,
                                 static_cast<int32_t>(layer) - c.base_layer);
        if (s.value == kRoleText || s.value == kRoleDeco) continue;  // texts / decorations take no clicks
        if (!has_art(c) && s.icon.empty()) {
            if (s.text.empty()) continue;
            sz = {40.0f, 14.0f};  // [port] clickable text without art
        }
        if (p.size.x > 0) sz = p.size;
        if (sz.x <= 0 || sz.y <= 0) sz = {40.0f, 40.0f};
        HudHitBox h;
        h.component = s.component;
        h.layer = layer;
        const Vec2f hc = p.size.x > 0 ? p.rect_center : p.center;
        h.min = {hc.x - 0.5f * sz.x, hc.y - 0.5f * sz.y};
        h.max = {hc.x + 0.5f * sz.x, hc.y + 0.5f * sz.y};
        hits_.push_back(h);
    }
}

void HudPanel::end(const PanelFrame& f) {
    if (!open_ || !table_) return;
    tooltip_.update(buttons_, *table_, tooltip_cfg_, f.time);
    if (fonts_) tooltip_.draw(dl_, *atlas_, *fonts_, canvas_, *table_, buttons_, text_, f.cursor_px, f.faction_alt, tooltip_cfg_);
}

std::vector<ButtonClick> HudPanel::clicks(const RoutedInput& routed, bool shift, bool ctrl, double now) {
    std::vector<ButtonClick> all, mine;
    if (!table_ || !open_) return mine;
    const int32_t hover = owns(routed.hover_component) ? routed.hover_component : -1;
    std::vector<HudPointerEvent> ev;
    for (const HudPointerEvent& e : routed.hud)
        if (owns(e.component)) ev.push_back(e);
    buttons_.update(hover, ev, shift, ctrl, now, all);
    for (const ButtonClick& k : all)
        if (owns(k.component)) mine.push_back(k);
    return mine;
}

namespace {
void close_action(std::vector<HudAction>* actions, const ComponentTable& t, int32_t shell, const ButtonClick& k) {
    if (!actions) return;
    HudAction a;
    a.kind = HudAction::Kind::PanelClose;
    a.component = k.component;
    a.name = shell >= 0 ? t[static_cast<size_t>(shell)].name : std::string();
    a.button = k.button;
    actions->push_back(std::move(a));
}
}  // namespace

// ==== Hero roster ==================================================================================================
void HeroPanel::fallback_layout() {
    // GALAXY_MAP_PRESENTATION §4 without the frame shells: the good bar pinned top-right growing left (slot 00 rightmost,
    // as the negative-x bones of i_main_hero_frame_rebel.alo), the evil bar top-left growing right.
    const float W = 0.5f * canvas_.width, H = 0.5f * canvas_.height;
    for (int side = 0; side < 2; ++side) {
        const char* s = side == 0 ? "good" : "evil";
        for (int k = 0; k <= 10; ++k) {
            const float d = 30.0f + 44.0f * static_cast<float>(k);
            const float x = side == 0 ? W - d : -W + d;
            put(fmts("hero_slot_%s%02d", s, k), x, H - 34.0f);
            put(fmts("hero_hb_%s%02d", s, k), x, H - 55.0f);
        }
    }
}

uint32_t hero_tie_key(std::string_view type_name) {
    // FoC type fix-up: CRC-32 of an _strupr copy of the name (ASCII upper case, C locale).
    const std::string up = ascii_upper(type_name);
    return alamo::crc32(up.data(), up.size());
}

void sort_hero_entries(std::vector<HeroEntry>& heroes) {
    std::vector<uint32_t> keys(heroes.size());
    for (size_t i = 0; i < heroes.size(); ++i) keys[i] = hero_tie_key(heroes[i].type_name);
    auto before = [](const HeroEntry& a, uint32_t ka, const HeroEntry& b, uint32_t kb) {
        if (a.ranking <= 0 || b.ranking <= 0) return false;
        if (a.ranking != b.ranking) return a.ranking < b.ranking;
        return ka < kb;
    };
    for (size_t i = 1; i < heroes.size(); ++i) {
        HeroEntry x = std::move(heroes[i]);
        const uint32_t kx = keys[i];
        size_t j = i;
        for (; j > 0 && before(x, kx, heroes[j - 1], keys[j - 1]); --j) {
            heroes[j] = std::move(heroes[j - 1]);
            keys[j] = keys[j - 1];
        }
        heroes[j] = std::move(x);
        keys[j] = kx;
    }
}

void HeroPanel::build(const HeroPanelFrame& f) {
    begin(f, f.good ? 0 : 1);
    good_ = f.good;
    tactical_ = f.tactical;
    shown_.assign(f.heroes.begin(), f.heroes.end());
    sort_hero_entries(shown_);
    if (shown_.size() > 11) shown_.resize(11);
    if (open_) {
        const char* s = f.good ? "good" : "evil";
        for (int k = 0; k <= 10; ++k) {
            const auto i = static_cast<size_t>(k);
            if (i >= shown_.size()) break;
            const HeroEntry& h = shown_[i];
            PanelSlot slot;
            slot.component = find(fmts("hero_slot_%s%02d", s, k));
            slot.icon = h.icon;
            slot.selected = h.selected;
            slot.dial = h.respawn;
            slot.id = h.object;
            slot.value = k;
            slots_.push_back(slot);
            PanelSlot bar;
            bar.component = find(fmts("hero_hb_%s%02d", s, k));
            bar.bar = h.respawn >= 0.0f ? -1.0f : h.health;
            bar.value = kRoleBar;
            slots_.push_back(bar);
        }
        draw_slots(f);
        // FIXPASS_GALACTIC §2.2 hover: the hero type's tooltip after Tooltip_Delay (no tooltip while a button is held).
        const int32_t hc = hover_component();
        if (hc >= 0 && fonts_ && buttons_.pressed() < 0 && f.time - buttons_.hover_since() + 1e-9 >= tooltip_cfg_.delay)
            if (const PanelSlot* sl = slot_of(hc); sl && sl->value >= 0 && static_cast<size_t>(sl->value) < shown_.size())
                TooltipState::draw_text(dl_, *atlas_, *fonts_, canvas_, *table_, shown_[static_cast<size_t>(sl->value)].tooltip,
                                        f.cursor_px, f.faction_alt, tooltip_cfg_);
    }
    end(f);
}

int32_t HeroPanel::hover_component() const {
    if (!open_ || !good_ || !table_) return -1;
    const int32_t h = buttons_.hover();
    const PanelSlot* s = h >= 0 ? slot_of(h) : nullptr;
    return s && s->value >= 0 ? h : -1;
}

uint32_t HeroPanel::hover_hero() const {
    const int32_t h = hover_component();
    const PanelSlot* s = h >= 0 ? slot_of(h) : nullptr;
    return s && s->id != commands::kNoObject ? s->id : 0u;
}

void HeroPanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>&,
                             std::vector<HudAction>* actions) {
    constexpr double kDoubleClickTime = 0.5;  // InputConfig::double_click_time
    constexpr int32_t kDoubleClickSlop = 4;   // InputConfig::double_click_slop_px
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        // §2.1: only hero_slot_good* have callbacks; no slot has a right-click action.
        if (!s || s->value < 0 || !good_ || k.button != Button::Left) continue;
        const bool dbl = k.component == last_click_component_ && now - last_click_time_ <= kDoubleClickTime &&
                         std::abs(k.x - last_click_x_) <= kDoubleClickSlop && std::abs(k.y - last_click_y_) <= kDoubleClickSlop;
        // A third click starts a new pair.
        last_click_component_ = dbl ? -1 : k.component;
        last_click_time_ = now;
        last_click_x_ = k.x;
        last_click_y_ = k.y;
        if (!actions || static_cast<size_t>(s->value) >= shown_.size()) continue;
        const HeroEntry& h = shown_[static_cast<size_t>(s->value)];
        if (h.object == 0 || h.object == commands::kNoObject) continue;  // building / respawning: no object
        auto emit = [&](HudAction::Kind kind, std::vector<uint32_t> ids) {
            HudAction a;
            a.kind = kind;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = h.object;
            a.ids = std::move(ids);
            a.button = k.button;
            a.shift = k.shift;
            a.ctrl = k.ctrl;
            actions->push_back(std::move(a));
        };
        if (!tactical_) {
            if (h.planet == commands::kNoObject || h.planet == 0) continue;  // hyperspace: no planet up the parent chain
            emit(HudAction::Kind::HeroFocusPlanet, {h.planet});
            if (dbl) emit(HudAction::Kind::HeroZoomPlanet, {h.planet});
        } else {
            emit(HudAction::Kind::HeroSelect, {h.object});
            if (dbl) {
                std::vector<uint32_t> ids;
                if (h.container != commands::kNoObject && h.container != 0) ids.push_back(h.container);
                emit(HudAction::Kind::HeroSelectGroup, std::move(ids));
            }
        }
    }
}

// ==== Black market =================================================================================================
void BlackMarketPanel::fallback_layout() {
    put("market_title00", -170.0f, 215.0f);
    put("market_title01", -170.0f, 195.0f);
    for (int k = 0; k < 5; ++k) {
        const float y = 140.0f - 62.0f * static_cast<float>(k);
        put(fmt("market_%02d", k), -200.0f, y);
        put(fmt("market_title%02d", 4 + 2 * k), -150.0f, y + 9.0f);
        put(fmt("market_title%02d", 5 + 2 * k), -150.0f, y - 9.0f);
        put(fmt("market_tech_level%02d", k), 170.0f, y);
    }
    put("market_title02", -200.0f, -185.0f);
    put("market_title03", 150.0f, -185.0f);
    put("market_close", 175.0f, -225.0f);
}

void BlackMarketPanel::build(const BlackMarketFrame& f) {
    begin(f, 0);
    planet_ = f.planet;
    if (open_) {
        auto text = [&](const char* name, std::u32string t) {
            PanelSlot s;
            s.component = find(name);
            s.text = std::move(t);
            s.value = kRoleText;
            slots_.push_back(std::move(s));
        };
        text("market_title00", f.title);
        text("market_title01", f.subtitle);
        int32_t hovered = -1;
        for (size_t k = 0; k < f.items.size() && k < 5; ++k)
            if (buttons_.hover() >= 0 && buttons_.hover() == find(fmt("market_%02d", static_cast<int>(k)))) hovered = static_cast<int32_t>(k);
        for (size_t k = 0; k < f.items.size() && k < 5; ++k) {
            const BlackMarketItem& it = f.items[k];
            const int ki = static_cast<int>(k);
            PanelSlot b;
            b.component = find(fmt("market_%02d", ki));
            b.icon = it.icon;
            b.disabled = !it.enabled;
            b.id = it.item;
            b.value = ki;
            slots_.push_back(b);
            text(fmt("market_title%02d", 4 + 2 * ki).c_str(), it.name);
            text(fmt("market_title%02d", 5 + 2 * ki).c_str(), it.cost >= 0 ? num32(it.cost) : std::u32string());
            if (it.tech_alt >= 0) {
                PanelSlot t;
                t.component = find(fmt("market_tech_level%02d", ki));
                t.alt = it.tech_alt;
                t.value = kRoleDeco;
                slots_.push_back(t);
            }
        }
        const BlackMarketItem* h = hovered >= 0 ? &f.items[static_cast<size_t>(hovered)] : nullptr;
        text("market_title02", h ? h->description : std::u32string());
        text("market_title03", h && h->cost >= 0 ? num32(h->cost) : std::u32string());
        PanelSlot close;
        close.component = find("market_close");
        close.value = kRoleClose;
        slots_.push_back(close);
        draw_slots(f);
    }
    end(f);
}

void BlackMarketPanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now,
                                    std::vector<commands::EventData>&, std::vector<HudAction>* actions) {
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        if (!s) continue;
        if (s->value == kRoleClose || k.button == Button::Right) {
            close_action(actions, *table_, active_shell(), k);
        } else if (s->value >= 0 && k.button == Button::Left && !s->disabled && planet_ != commands::kNoObject && actions) {
            HudAction a;
            a.kind = HudAction::Kind::BlackMarketBuy;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = s->id;
            a.value = s->value;
            a.ids = {planet_};
            actions->push_back(std::move(a));
        }
    }
}

// ==== Slice dialog ==================================================================================================
void SlicePanel::fallback_layout() {
    // [port] the market_frame stand-in: same positions as BlackMarketPanel::fallback_layout.
    put("market_title00", -170.0f, 215.0f);
    put("market_title01", -170.0f, 195.0f);
    for (int k = 0; k < kVisibleRows; ++k) {
        const float y = 140.0f - 62.0f * static_cast<float>(k);
        put(fmt("market_%02d", k), -200.0f, y);
        put(fmt("market_title%02d", 4 + 2 * k), -150.0f, y + 9.0f);
        put(fmt("market_title%02d", 5 + 2 * k), -150.0f, y - 9.0f);
    }
    put("market_title02", -200.0f, -185.0f);
    put("market_title03", 150.0f, -185.0f);
    put("market_close", 175.0f, -225.0f);
}

void SlicePanel::build(const SliceFrame& f) {
    begin(f, 0);
    planet_ = f.planet;
    slicer_ = f.slicer;
    rows_ = static_cast<int32_t>(std::min<size_t>(f.rows.size(), 0x7FFFFFFF));
    if (open_ && !was_open_) first_ = 0;  // a new dialog starts at its first row
    was_open_ = open_;
    first_ = std::clamp(first_, 0, std::max(0, rows_ - kVisibleRows));
    if (open_) {
        auto text = [&](const std::string& name, std::u32string t) {
            PanelSlot s;
            s.component = find(name);
            s.text = std::move(t);
            s.value = kRoleText;
            slots_.push_back(std::move(s));
        };
        auto chance_text = [](const SliceRow& r) {
            if (!r.description.empty() || r.chance < 0.0f) return r.description;
            return U"Chance " + num32(std::lround(std::clamp(r.chance, 0.0f, 1.0f) * 100.0f)) + U"%";  // [port] wording
        };
        text("market_title00", f.title);
        text("market_title01", f.description);
        const SliceRow* hovered = nullptr;
        for (int32_t k = 0; k < kVisibleRows; ++k) {
            const int32_t row = first_ + k;
            const std::string pad = fmt("market_%02d", k);
            const std::string name = fmt("market_title%02d", 4 + 2 * k), price = fmt("market_title%02d", 5 + 2 * k);
            if (row >= rows_) {  // unused rows: blank texts, no button
                text(name, {});
                text(price, {});
                continue;
            }
            const SliceRow& r = f.rows[static_cast<size_t>(row)];
            if (buttons_.hover() >= 0 && buttons_.hover() == find(pad)) hovered = &r;
            PanelSlot b;
            b.component = find(pad);
            b.icon = r.icon;
            b.disabled = !r.enabled;
            b.id = r.type;
            b.value = row;
            slots_.push_back(std::move(b));
            text(name, r.name);
            text(price, r.cost >= 0 ? num32(r.cost) : std::u32string());
        }
        text("market_title02", hovered ? chance_text(*hovered) : std::u32string());
        text("market_title03", hovered && hovered->cost >= 0 ? num32(hovered->cost) : std::u32string());
        PanelSlot close;
        close.component = find("market_close");
        close.value = kRoleClose;
        slots_.push_back(close);
        draw_slots(f);
    }
    end(f);
}

void SlicePanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>&,
                              std::vector<HudAction>* actions, float wheel) {
    if (open_ && wheel != 0.0f && routed.wheel_target == PointerTarget::Hud && owns(routed.hover_component)) {
        const int32_t notches = static_cast<int32_t>(std::lround(wheel));
        first_ = std::clamp(first_ - notches, 0, std::max(0, rows_ - kVisibleRows));  // + = away from the user = up
    }
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        if (!s) continue;
        if (s->value == kRoleClose || k.button == Button::Right) {
            close_action(actions, *table_, active_shell(), k);
        } else if (s->value >= 0 && k.button == Button::Left && !s->disabled && planet_ != commands::kNoObject && actions) {
            HudAction a;
            a.kind = HudAction::Kind::SliceSelect;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = s->id;
            a.value = s->value;
            a.ids = {planet_, slicer_};
            actions->push_back(std::move(a));
        }
    }
}

// ==== Sabotage =====================================================================================================
void SabotagePanel::fallback_layout() {
    put("bm_title", -150.0f, 215.0f);
    put("bm_title01", -150.0f, 195.0f);
    put("bm_text_steal", -150.0f, 175.0f);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 2; ++c) {
            const float x = -150.0f + 200.0f * static_cast<float>(c), y = 120.0f - 62.0f * static_cast<float>(r);
            put(fmt2("bm_%02d0%d", r, c), x, y);
            put(fmt2("bm_title_%d0%d", r, c), x + 50.0f, y - 9.0f);
            put(fmt("bm_title_40%d", r * 2 + c + 2), x + 50.0f, y + 9.0f);
        }
    put("bm_close", 175.0f, -225.0f);
}

void SabotagePanel::build(const SabotageFrame& f) {
    begin(f, 0);
    planet_ = f.planet;
    if (open_) {
        auto text = [&](const std::string& name, std::u32string t) {
            PanelSlot s;
            s.component = find(name);
            s.text = std::move(t);
            s.value = kRoleText;
            slots_.push_back(std::move(s));
        };
        text("bm_title", f.title);
        text("bm_title01", f.subtitle);
        text("bm_text_steal", f.steal_text);
        for (size_t k = 0; k < f.targets.size() && k < 10; ++k) {
            const SabotageTarget& t = f.targets[k];
            const int r = static_cast<int>(k) / 2, c = static_cast<int>(k) % 2;
            PanelSlot b;
            b.component = find(fmt2("bm_%02d0%d", r, c));
            b.icon = t.icon;
            b.disabled = !t.enabled;
            b.id = t.object;
            b.value = static_cast<int32_t>(k);
            slots_.push_back(b);
            text(fmt2("bm_title_%d0%d", r, c), t.cost >= 0 ? num32(t.cost) : std::u32string());
            text(fmt("bm_title_40%d", static_cast<int>(k) + 2), t.name);
        }
        PanelSlot close;
        close.component = find("bm_close");
        close.value = kRoleClose;
        slots_.push_back(close);
        draw_slots(f);
    }
    end(f);
}

void SabotagePanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>&,
                                 std::vector<HudAction>* actions) {
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        if (!s) continue;
        if (s->value == kRoleClose || k.button == Button::Right) {
            close_action(actions, *table_, active_shell(), k);
        } else if (s->value >= 0 && k.button == Button::Left && !s->disabled && actions) {
            HudAction a;
            a.kind = HudAction::Kind::SabotageTarget;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = s->id;
            a.ids = {planet_};
            actions->push_back(std::move(a));
        }
    }
}

// ==== Corruption ===================================================================================================
void CorruptionPanel::fallback_layout() {
    put("corrupt_planet_title00", -200.0f, 225.0f);
    put("corrupt_planet_title01", -200.0f, 207.0f);
    put("corrupt_planet_defiler_00", -200.0f, 189.0f);
    put("corrupt_planet_defiler_01", -200.0f, 173.0f);
    const char* abc = "abc";
    for (int i = 0; i < 3; ++i) {
        const float x = -220.0f + 220.0f * static_cast<float>(i);
        const std::string x1(1, abc[i]);
        put("corrupt_button_icon_" + x1, x, 120.0f);
        put("corrupt_title_" + x1, x + 40.0f, 130.0f);
        put("corrupt_cost_" + x1, x + 40.0f, 112.0f);
        for (int n = 0; n < 5; ++n) put(fmt("corrupt_text%02d", n) + x1, x - 20.0f, 70.0f - 16.0f * static_cast<float>(n));
        for (int n = 0; n < 8; ++n) put(fmt("corrupt_icon_%02d", n) + x1, x - 20.0f + 26.0f * static_cast<float>(n % 4), -40.0f - 26.0f * static_cast<float>(n / 4));
    }
    put("corrupt_close", 175.0f, -225.0f);
}

void CorruptionPanel::build(const CorruptionFrame& f) {
    begin(f, 0);
    planet_ = f.planet;
    if (open_) {
        auto text = [&](const std::string& name, std::u32string t) {
            PanelSlot s;
            s.component = find(name);
            s.text = std::move(t);
            s.value = kRoleText;
            slots_.push_back(std::move(s));
        };
        text("corrupt_planet_title00", f.planet_title);
        text("corrupt_planet_title01", f.planet_subtitle);
        text("corrupt_planet_defiler_00", f.defiler_text);
        text("corrupt_planet_defiler_01", f.defiler_text2);
        const char* abc = "abc";
        for (size_t i = 0; i < f.options.size() && i < 3; ++i) {
            const CorruptionOption& o = f.options[i];
            const std::string x1(1, abc[i]);
            PanelSlot b;
            b.component = find("corrupt_button_icon_" + x1);
            b.icon = o.icon;
            b.disabled = !o.enabled;
            b.id = static_cast<uint32_t>(i);
            b.value = o.corruption_type >= 0 ? o.corruption_type : 0;
            slots_.push_back(b);
            text("corrupt_title_" + x1, o.title);
            text("corrupt_cost_" + x1, o.cost >= 0 ? num32(o.cost) : std::u32string());
            for (size_t n = 0; n < o.lines.size() && n < 5; ++n) text(fmt("corrupt_text%02d", static_cast<int>(n)) + x1, o.lines[n]);
            for (size_t n = 0; n < o.requirements.size() && n < 8; ++n) {
                PanelSlot r;
                r.component = find(fmt("corrupt_icon_%02d", static_cast<int>(n)) + x1);
                r.icon = o.requirements[n];
                r.value = kRoleDeco;
                slots_.push_back(r);
            }
        }
        PanelSlot close;
        close.component = find("corrupt_close");
        close.value = kRoleClose;
        slots_.push_back(close);
        draw_slots(f);
    }
    end(f);
}

void CorruptionPanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now,
                                   std::vector<commands::EventData>&, std::vector<HudAction>* actions) {
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        if (!s) continue;
        if (s->value == kRoleClose || k.button == Button::Right) {
            close_action(actions, *table_, active_shell(), k);
        } else if (s->value >= 0 && k.button == Button::Left && !s->disabled && actions) {
            HudAction a;
            a.kind = HudAction::Kind::CorruptionChoice;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = planet_;
            a.value = s->value;
            actions->push_back(std::move(a));
        }
    }
}

// ==== Hack =========================================================================================================
void HackPanel::fallback_layout() {
    put("hack_title01", -150.0f, 200.0f);
    for (int n = 0; n < 7; ++n) put(fmt("hack_text_%02d", n), -150.0f, 160.0f - 18.0f * static_cast<float>(n));
    put("hack_cost", 120.0f, -60.0f);
    put("hack_hack", 0.0f, -110.0f);
    put("hack_close", 175.0f, -160.0f);
}

void HackPanel::build(const HackFrame& f) {
    begin(f, 0);
    target_ = f.target;
    planet_ = f.planet;
    if (open_) {
        auto text = [&](const std::string& name, std::u32string t) {
            PanelSlot s;
            s.component = find(name);
            s.text = std::move(t);
            s.value = kRoleText;
            slots_.push_back(std::move(s));
        };
        text("hack_title01", f.title);
        for (size_t n = 0; n < f.lines.size() && n < 7; ++n) text(fmt("hack_text_%02d", static_cast<int>(n)), f.lines[n]);
        text("hack_cost", f.cost);
        PanelSlot h;
        h.component = find("hack_hack");
        h.text = f.confirm_text;
        h.disabled = !f.enabled;
        h.id = f.target;
        h.value = 0;
        slots_.push_back(h);
        PanelSlot close;
        close.component = find("hack_close");
        close.value = kRoleClose;
        slots_.push_back(close);
        draw_slots(f);
    }
    end(f);
}

void HackPanel::update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>&,
                             std::vector<HudAction>* actions) {
    for (const ButtonClick& k : clicks(routed, shift, ctrl, now)) {
        const PanelSlot* s = slot_of(k.component);
        if (!s) continue;
        if (s->value == kRoleClose || k.button == Button::Right) {
            close_action(actions, *table_, active_shell(), k);
        } else if (s->value == 0 && k.button == Button::Left && !s->disabled && actions) {
            HudAction a;
            a.kind = HudAction::Kind::HackConfirm;
            a.component = k.component;
            a.name = (*table_)[static_cast<size_t>(k.component)].name;
            a.unit = target_;
            a.ids = {planet_};
            actions->push_back(std::move(a));
        }
    }
}

}  // namespace alamo::ui
