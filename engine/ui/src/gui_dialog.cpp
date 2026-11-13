#include "alamo/ui/gui_dialog.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/hash.hpp"
#include "alamo/ui/hud_font.hpp"

namespace alamo::ui {

namespace {

bool focusable(const GuiControl& c) {
    return c.def && c.def->tabstop && c.visible && c.enabled;
}

bool clickable(GuiGadgetType t) {
    return t == GuiGadgetType::PushButton || t == GuiGadgetType::CheckBox || t == GuiGadgetType::RadioButton;
}

constexpr uint32_t kPopupLayer = 12;
constexpr float kScrollWidth = 12.0f;  // [port] list scroll bar column width (canvas units) when no texture size
constexpr float kFieldInset = 5.0f;    // [port] QA D8 / D10: left padding of edit and closed combo text (canvas units)
constexpr float kCheckGlyph = 13.0f;   // [port] QA D8: one check box size; the shipped rows are 10 to 23 units tall

}  // namespace

// ---- GuiImagePages ----------------------------------------------------------------------------------------
int32_t GuiImagePages::page(std::string_view texture) {
    const std::string key = atlas_key(texture);
    ++clock_;
    for (Entry& e : entries_)
        if (!e.failed && atlas_key(e.name) == key) {
            e.last_use = clock_;
            return e.page;
        }
    for (const std::string& f : failed_)
        if (f == key) return -1;
    if (entries_.size() < kHudImagePages) {
        Entry e;
        e.name = std::string(texture);
        e.page = static_cast<uint16_t>(kHudPageImageBase + entries_.size());
        e.last_use = clock_;
        entries_.push_back(e);
        return e.page;
    }
    // Full: reuse a failed slot, else the least recently requested one (lowest page on ties: deterministic).
    Entry* victim = nullptr;
    for (Entry& e : entries_)
        if (!victim || (e.failed && !victim->failed) || (e.failed == victim->failed && e.last_use < victim->last_use))
            victim = &e;
    victim->name = std::string(texture);
    victim->width = victim->height = 0;
    victim->failed = false;
    victim->last_use = clock_;
    return victim->page;
}

void GuiImagePages::set_loaded(uint16_t page, uint32_t width, uint32_t height) {
    for (Entry& e : entries_)
        if (e.page == page) {
            e.width = width;
            e.height = height;
            e.failed = false;
        }
}

void GuiImagePages::set_failed(uint16_t page) {
    for (Entry& e : entries_)
        if (e.page == page && !e.failed) {
            e.failed = true;
            failed_.push_back(atlas_key(e.name));
        }
}

GuiImage GuiImagePages::resolve(std::string_view texture) {
    const int32_t p = page(texture);
    GuiImage img;
    if (p < 0) return img;
    for (const Entry& e : entries_)
        if (e.page == p && e.width > 0 && !e.failed) {
            img.valid = true;
            img.page = e.page;
            img.size = {static_cast<float>(e.width), static_cast<float>(e.height)};
        }
    return img;
}

// ---- GuiDialog -----------------------------------------------------------------------------------------------
GuiControl* GuiDialog::get(std::string_view id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &controls_[static_cast<size_t>(i)];
}
const GuiControl* GuiDialog::get(std::string_view id) const {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &controls_[static_cast<size_t>(i)];
}

bool GuiDialog::set_text(std::string_view id, std::u32string text) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->text = std::move(text);
    c->caret = std::min(c->caret, c->text.size());
    c->sel_anchor = c->caret;
    c->scroll_x = 0;
    return true;
}
bool GuiDialog::show(std::string_view id, bool visible) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->visible = visible;
    if (!visible && focus_ == find(id)) focus_ = -1;
    return true;
}
bool GuiDialog::enable(std::string_view id, bool enabled) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->enabled = enabled;
    return true;
}
bool GuiDialog::set_checked(std::string_view id, bool checked) {
    GuiControl* c = get(id);
    if (!c) return false;
    if (checked && c->type == GuiGadgetType::RadioButton)
        for (GuiControl& o : controls_)
            if (&o != c && o.type == GuiGadgetType::RadioButton && o.radio_group == c->radio_group) o.checked = false;
    c->checked = checked;
    return true;
}
bool GuiDialog::set_value(std::string_view id, int32_t value) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->value = std::clamp(value, c->range_min, c->range_max);
    return true;
}
bool GuiDialog::set_range(std::string_view id, int32_t lo, int32_t hi) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->range_min = std::min(lo, hi);
    c->range_max = std::max(lo, hi);
    c->value = std::clamp(c->value, c->range_min, c->range_max);
    return true;
}
bool GuiDialog::set_items(std::string_view id, std::vector<std::u32string> items, int32_t selected) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->items = std::move(items);
    c->selected = selected >= 0 && static_cast<size_t>(selected) < c->items.size() ? selected : -1;
    c->top = 0;
    return true;
}
bool GuiDialog::set_selected(std::string_view id, int32_t index) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->selected = index >= 0 && static_cast<size_t>(index) < c->items.size() ? index : -1;
    return true;
}
bool GuiDialog::set_image(std::string_view id, std::string texture) {
    GuiControl* c = get(id);
    if (!c) return false;
    c->image = std::move(texture);
    return true;
}
bool GuiDialog::focus(std::string_view id) {
    const int32_t i = find(id);
    if (i < 0) return false;
    focus_ = i;
    keyboard_focus_ = true;
    return true;
}

GuiRect GuiDialog::rect() const {
    GuiRect r;
    r.min = {origin_.x, origin_.y - height()};
    r.max = {origin_.x + static_cast<float>(def_->w), origin_.y};
    return r;
}

GuiRect GuiDialog::control_rect(size_t i) const {
    const RcControl& c = *controls_[i].def;
    GuiRect r;
    const Vec2f off = controls_[i].offset, sc = controls_[i].scale;
    r.min.x = origin_.x + static_cast<float>(c.x) + off.x;
    r.max.y = origin_.y - static_cast<float>(c.y) + off.y;
    r.max.x = r.min.x + static_cast<float>(c.w) * sc.x;
    r.min.y = r.max.y - static_cast<float>(c.h) * sc.y;
    return r;
}

int32_t GuiDialog::control_at(Vec2f p) const {
    // Topmost = last in script order (later controls draw over earlier ones).
    for (size_t k = controls_.size(); k-- > 0;) {
        const GuiControl& c = controls_[k];
        if (!c.visible || c.type == GuiGadgetType::Frame || c.type == GuiGadgetType::Unknown) continue;
        if (c.type == GuiGadgetType::Text || c.type == GuiGadgetType::ProgressBar || c.type == GuiGadgetType::CustomImage)
            continue;  // [port] passive gadgets never take the pointer
        GuiRect r = control_rect(k);
        if (c.type == GuiGadgetType::ComboBox) {  // closed field only (the RC height includes the drop-down)
            const float field = std::min(r.height(), c.field_height);
            r.min.y = r.max.y - field;
        }
        if (r.contains(p)) return static_cast<int32_t>(k);
    }
    return -1;
}

// ---- GuiManager ------------------------------------------------------------------------------------------------
void GuiManager::init(const DialogScript& script, const GuiSkin& skin, const TextureAtlas& atlas, FontCache& fonts,
                      const TextDatabase* text) {
    script_ = &script;
    skin_ = &skin;
    atlas_ = &atlas;
    fonts_ = &fonts;
    text_ = text;
}

void GuiManager::resize(uint32_t width, uint32_t height) {
    const HudCanvas next = HudCanvas::make(width, height);
    if (fonts_ && next.screen_height != canvas_.screen_height) fonts_->clear();
    canvas_ = next;
    for (auto& d : stack_) place(*d);
}

std::u32string gui_unescape(std::u32string s) {
    // MasterTextFile strings carry C-style "\n" escapes (e.g. campaign descriptions) [port: decoded at display].
    std::u32string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == U'\\' && i + 1 < s.size() && (s[i + 1] == U'n' || s[i + 1] == U'r' || s[i + 1] == U't')) {
            o.push_back(s[i + 1] == U't' ? U' ' : U'\n');
            if (s[i + 1] == U'r' && i + 3 < s.size() && s[i + 2] == U'\\' && s[i + 3] == U'n') i += 2;  // \r\n
            ++i;
            continue;
        }
        o.push_back(s[i]);
    }
    return o;
}

std::u32string gui_no_asset_name(std::u32string s) {
    if (s.find(U'\\') != std::u32string::npos || s.find(U'/') != std::u32string::npos) return {};
    const size_t dot = s.rfind(U'.');
    if (dot == std::u32string::npos || s.size() - dot != 4) return s;  // ".ted", ".tga", ".dds", ".alo", ".xml", ...
    for (size_t k = dot + 1; k < s.size(); ++k)
        if (!((s[k] >= U'a' && s[k] <= U'z') || (s[k] >= U'A' && s[k] <= U'Z'))) return s;
    return {};
}

std::vector<std::u32string> fit_text_lines(FontCache& fonts, int32_t font, std::u32string_view text, float max_width_px,
                                           int32_t max_lines, size_t keep_tail) {
    std::vector<std::u32string> lines = wrap_text(fonts, font, text, max_width_px);
    const auto keep = static_cast<size_t>(std::max(1, max_lines));
    if (lines.size() <= keep) return lines;
    keep_tail = std::min(keep_tail, text.size());
    const std::u32string cut = U"..." + std::u32string(text.substr(text.size() - keep_tail));
    if (keep_tail > 0) lines = wrap_text(fonts, font, text.substr(0, text.size() - keep_tail), max_width_px);
    if (lines.empty()) lines.emplace_back();
    if (lines.size() > keep) lines.resize(keep);
    std::u32string& last = lines.back();
    auto wide = [&](const std::u32string& t) { return static_cast<float>(fonts.measure(font, t + cut)) > max_width_px; };
    while (!last.empty() && wide(last)) last.pop_back();
    while (!last.empty() && last.back() == U' ') last.pop_back();
    last += cut;
    return lines;
}

std::u32string GuiManager::caption_text(std::string_view caption) const {
    if (text_ && !caption.empty())
        if (const std::u16string* v = text_->find(caption)) return gui_unescape(utf16_to_utf32(*v));
    std::u32string as_written = utf8_to_utf32(caption);
    return text_ && !caption.starts_with("TEXT_") ? text_->port_text(as_written) : as_written;  // a missing key stays readable
}

std::u32string GuiManager::port_text(std::u32string_view english) const {
    return text_ ? text_->port_text(english) : std::u32string(english);
}

void GuiManager::place(GuiDialog& d) const {
    if (d.placed_) return;
    const auto w = static_cast<float>(d.def().w);
    const float h = d.height();
    if (h > canvas_.height) {
        // Menu column: top edge, right edge on the screen's right edge (base 20.jpg at 16:9: the buttons end ~1 % from the
        // right edge; UI.md §3.3 at 4:3). Its IDC_MENU_PETRO_LOGO hotspot follows the stretched backdrop (build()).
        d.origin_ = {0.5f * canvas_.width - w, 0.5f * canvas_.height};
    } else {
        d.origin_ = {std::round(-0.5f * w), std::round(0.5f * h)};  // centred [port]
    }
}

GuiDialog* GuiManager::open(std::string_view name, GuiOpenOptions opt) {
    if (!script_) return nullptr;
    for (size_t i = 0; i < stack_.size(); ++i) {
        if (iequals(stack_[i]->name(), name)) {
            std::unique_ptr<GuiDialog> d = std::move(stack_[i]);
            stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
            d->opt_ = opt;
            stack_.push_back(std::move(d));
            return stack_.back().get();
        }
    }
    const RcDialog* def = script_->find(name);
    if (!def) return nullptr;
    auto d = std::make_unique<GuiDialog>(*def, opt);
    int32_t group = 0;
    d->controls_.reserve(def->controls.size());
    for (const RcControl& rc : def->controls) {
        GuiControl c;
        c.def = &rc;
        c.type = rc.gadget;
        c.visible = rc.visible;
        c.enabled = !rc.disabled;
        if (rc.group) ++group;
        if (c.type == GuiGadgetType::RadioButton) c.radio_group = group;
        switch (c.type) {
            case GuiGadgetType::PushButton:
            case GuiGadgetType::Text:
            case GuiGadgetType::CheckBox:
            case GuiGadgetType::RadioButton: c.text = caption_text(rc.caption); break;
            case GuiGadgetType::Frame:  // group box captions are placeholders unless they are text keys [port]
                if (text_ && text_->find(rc.caption)) c.text = caption_text(rc.caption);
                break;
            default: break;
        }
        d->controls_.push_back(std::move(c));
    }
    for (size_t i = 0; i < d->controls_.size(); ++i)
        if (d->controls_[i].type == GuiGadgetType::ComboBox) d->controls_[i].field_height = combo_field_height(*d, i);
    place(*d);
    stack_.push_back(std::move(d));
    return stack_.back().get();
}

void GuiManager::close(std::string_view name) {
    for (size_t i = 0; i < stack_.size(); ++i) {
        if (!iequals(stack_[i]->name(), name)) continue;
        GuiDialog* p = stack_[i].get();
        if (hover_dialog_ == p) {
            hover_dialog_ = nullptr;
            hover_control_ = -1;
        }
        if (press_dialog_ == p) {
            press_dialog_ = nullptr;
            press_control_ = -1;
            slider_drag_ = false;
        }
        stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void GuiManager::close_all() {
    stack_.clear();
    hover_dialog_ = press_dialog_ = nullptr;
    hover_control_ = press_control_ = -1;
    slider_drag_ = false;
}

GuiDialog* GuiManager::find(std::string_view name) {
    for (auto& d : stack_)
        if (iequals(d->name(), name)) return d.get();
    return nullptr;
}
const GuiDialog* GuiManager::find(std::string_view name) const {
    for (const auto& d : stack_)
        if (iequals(d->name(), name)) return d.get();
    return nullptr;
}

std::vector<std::string> GuiManager::open_names() const {
    std::vector<std::string> n;
    for (const auto& d : stack_) n.emplace_back(d->name());
    return n;
}

GuiDialog* GuiManager::modal_top() {
    for (size_t i = stack_.size(); i-- > 0;)
        if (stack_[i]->opt_.modal) return stack_[i].get();
    return nullptr;
}
const GuiDialog* GuiManager::modal_top() const {
    for (size_t i = stack_.size(); i-- > 0;)
        if (stack_[i]->opt_.modal) return stack_[i].get();
    return nullptr;
}

bool GuiManager::hit(int32_t px, int32_t py) const {
    if (stack_.empty()) return false;
    const Vec2f p = canvas_.pixel_to_canvas(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f);
    const GuiDialog& t = *stack_.back();
    if (t.opt_.modal || t.rect().contains(p)) return true;
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].open && combo_list(t, i).contains(p)) return true;
    for (const auto& d : stack_)
        if (d->opt_.always_input && d->rect().contains(p)) return true;
    return false;
}

bool GuiManager::has_focus() const { return modal_top() != nullptr; }

void GuiManager::install(RouteHooks& hooks) const {
    hooks.dialog_hit = [this](int32_t x, int32_t y) { return hit(x, y); };
    hooks.dialog_focus = [this]() { return has_focus(); };
}

GuiEvent GuiManager::make_event(GuiEvent::Kind k, const GuiDialog& d, int32_t control, int32_t value) const {
    GuiEvent e;
    e.kind = k;
    e.dialog = std::string(d.name());
    e.control_index = control;
    if (control >= 0) e.control = std::string(d[static_cast<size_t>(control)].id());
    e.value = value;
    return e;
}

void GuiManager::sfx(std::string_view e, int32_t control) const {
    if (sfx_) sfx_(e, control);
}

float GuiManager::row_height(const GuiDialog& d, size_t i) const {
    const int32_t f = control_font(d, i);
    if (f < 0 || !fonts_) return 12.0f;
    return static_cast<float>(std::max(1, fonts_->metrics(f).line_height)) * canvas_.units_per_pixel_y;
}

int32_t GuiManager::visible_rows(const GuiDialog& d, size_t i) const {
    const float rh = row_height(d, i);
    const GuiRect r = d.control_rect(i);
    return std::max(1, static_cast<int32_t>((r.height() - 4.0f) / rh));
}

float GuiManager::combo_field_height(const GuiDialog& d, size_t i) const {
    if (!skin_) return 15.0f;
    const GuiImage b = image(skin_->texture(d.name(), d[i].id(), "Combo_Box_Popdown_Button"));
    return b.valid && b.size.y > 0.0f ? b.size.y : 15.0f;  // UI.md §3.7.1: 22 x 25 button, 2 x 25 caps
}

GuiRect GuiManager::combo_field(const GuiDialog& d, size_t i) const {
    GuiRect r = d.control_rect(i);
    r.min.y = r.max.y - std::min(r.height(), combo_field_height(d, i));
    return r;
}

int32_t GuiManager::combo_rows(const GuiDialog& d, size_t i) const {
    const float rh = row_height(d, i);
    const float avail = std::max(rh, d.control_rect(i).height() - combo_field(d, i).height());
    return std::max(1, std::min(static_cast<int32_t>(d[i].items.size()), static_cast<int32_t>(avail / rh)));
}

GuiRect GuiManager::combo_list(const GuiDialog& d, size_t i) const {
    const GuiRect f = combo_field(d, i);
    const float rh = row_height(d, i);
    const int32_t rows = combo_rows(d, i);
    GuiRect r;
    r.max = {f.max.x, f.min.y};
    r.min = {f.min.x, f.min.y - static_cast<float>(rows) * rh - 4.0f};
    return r;
}

int32_t GuiManager::scroll_rows(const GuiDialog& d, size_t i) const {
    return d[i].type == GuiGadgetType::ComboBox ? combo_rows(d, i) : visible_rows(d, i);
}

float GuiManager::scroll_width(const GuiDialog& d, size_t i) const {
    if (!skin_) return kScrollWidth;
    const GuiImage up = image(skin_->texture(d.name(), d[i].id(), "Scroll_Up_Button"));
    return up.valid && up.size.x > 0.0f ? up.size.x : kScrollWidth;
}

GuiManager::ScrollBar GuiManager::scroll_bar(const GuiDialog& d, size_t i) const {
    ScrollBar sb;
    const GuiControl& c = d[i];
    const GuiRect area = c.type == GuiGadgetType::ComboBox ? combo_list(d, i) : d.control_rect(i);
    const auto count = static_cast<int32_t>(c.items.size());
    sb.vis = scroll_rows(d, i);
    sb.active = count > sb.vis;
    sb.max_top = std::max(0, count - sb.vis);
    const float sw = std::min(scroll_width(d, i), area.width());
    float ah = sw;  // arrow button height: the art's aspect at the column width
    if (skin_) {
        const GuiImage up = image(skin_->texture(d.name(), c.id(), "Scroll_Up_Button"));
        if (up.valid && up.size.x > 0.0f) ah = sw * up.size.y / up.size.x;
    }
    ah = std::min(ah, 0.5f * area.height());
    sb.col = {{area.max.x - sw, area.min.y}, area.max};
    sb.up = {{sb.col.min.x, sb.col.max.y - ah}, sb.col.max};
    sb.down = {sb.col.min, {sb.col.max.x, sb.col.min.y + ah}};
    sb.track_top = sb.up.min.y;
    sb.track_len = std::max(0.0f, sb.col.height() - 2.0f * ah);
    const float th = count > 0 ? std::clamp(sb.track_len * static_cast<float>(sb.vis) / static_cast<float>(count),
                                            std::min(sw, sb.track_len), sb.track_len)
                               : sb.track_len;
    const float ty = sb.track_top - (sb.track_len - th) * static_cast<float>(std::clamp(c.top, 0, sb.max_top)) /
                                        static_cast<float>(std::max(1, sb.max_top));
    sb.thumb = {{sb.col.min.x, ty - th}, {sb.col.max.x, ty}};
    return sb;
}

GuiManager::ScrollPart GuiManager::scroll_part_at(const ScrollBar& sb, Vec2f p) const {
    if (!sb.active || !sb.col.contains(p)) return ScrollPart::None;
    if (sb.up.contains(p)) return ScrollPart::Up;
    if (sb.down.contains(p)) return ScrollPart::Down;
    if (p.y > sb.thumb.max.y) return ScrollPart::PageUp;
    if (p.y < sb.thumb.min.y) return ScrollPart::PageDown;
    return ScrollPart::Thumb;
}

void GuiManager::scroll_press(GuiDialog& d, size_t i, ScrollPart part, double now) {
    GuiControl& c = d.controls_[i];
    const ScrollBar sb = scroll_bar(d, i);
    switch (part) {
        case ScrollPart::Up: scroll_list(c, i, d, -1); break;
        case ScrollPart::Down: scroll_list(c, i, d, 1); break;
        case ScrollPart::PageUp: scroll_list(c, i, d, -sb.vis); break;
        case ScrollPart::PageDown: scroll_list(c, i, d, sb.vis); break;
        case ScrollPart::Thumb: scroll_grab_ = sb.thumb.max.y - cursor_.y; break;
        case ScrollPart::None: return;
    }
    scroll_part_ = part;
    scroll_repeat_at_ = now + style_.scroll_repeat_delay;
    press_dialog_ = &d;
    press_control_ = static_cast<int32_t>(i);
}

void GuiManager::scroll_held(double now) {
    if (scroll_part_ == ScrollPart::None || !press_dialog_ || press_control_ < 0) return;
    GuiDialog& d = *press_dialog_;
    const auto i = static_cast<size_t>(press_control_);
    if (i >= d.size()) return;
    GuiControl& c = d.controls_[i];
    if (c.type == GuiGadgetType::ComboBox && !c.open) return;
    const ScrollBar sb = scroll_bar(d, i);
    if (!sb.active) return;
    if (scroll_part_ == ScrollPart::Thumb) {
        const float free = sb.track_len - sb.thumb.height();
        const float down = (sb.track_top - cursor_.y) - scroll_grab_;  // wanted thumb top, below the track top
        const float t = free > 0.0f ? std::clamp(down / free, 0.0f, 1.0f) : 0.0f;
        c.top = std::clamp(static_cast<int32_t>(std::lround(t * static_cast<float>(sb.max_top))), 0, sb.max_top);
        return;
    }
    if (now < scroll_repeat_at_) return;
    scroll_repeat_at_ = now + style_.scroll_repeat_rate;
    if (scroll_part_at(sb, cursor_) != scroll_part_) return;  // off the arrow, or the thumb reached the cursor
    const int32_t step = scroll_part_ == ScrollPart::Up ? -1 : scroll_part_ == ScrollPart::Down ? 1
                       : scroll_part_ == ScrollPart::PageUp ? -sb.vis : sb.vis;
    scroll_list(c, i, d, step);
}

void GuiManager::scroll_list(GuiControl& c, size_t i, const GuiDialog& d, int32_t rows) {
    const int32_t vis = scroll_rows(d, i);
    const int32_t max_top = std::max(0, static_cast<int32_t>(c.items.size()) - vis);
    c.top = std::clamp(c.top + rows, 0, max_top);
}

bool GuiManager::wrap_list_items(GuiDialog& d, std::string_view control) {
    const int32_t i = d.find(control);
    if (i < 0 || static_cast<size_t>(i) >= d.controls_.size() || !fonts_) return false;
    const auto ui = static_cast<size_t>(i);
    GuiControl& c = d.controls_[ui];
    if (c.type != GuiGadgetType::ListBox) return false;
    const int32_t font = control_font(d, ui);
    if (font < 0) return true;
    // Text rect of a row: list minus the scroll column, 2 + 2 units of inset (draw_control).
    const float units = d.control_rect(ui).width() - scroll_width(d, ui) - 6.0f;
    const float max_px = units / canvas_.units_per_pixel_x;
    if (max_px <= 0.0f) return true;
    std::vector<std::u32string> items;
    std::vector<Rgba8> colors, text_colors;
    for (size_t k = 0; k < c.items.size(); ++k) {
        // GALACTIC_DIALOGS §2.7: a backslash-n escape in the text becomes a line break here, where the text is wrapped.
        std::vector<std::u32string> rows = wrap_text(*fonts_, font, gui_unescape(c.items[k]), max_px);
        if (rows.empty()) rows.emplace_back();
        for (std::u32string& r : rows) {
            items.push_back(std::move(r));
            if (k < c.item_colors.size()) colors.push_back(c.item_colors[k]);
            if (k < c.item_text_colors.size()) text_colors.push_back(c.item_text_colors[k]);
            else if (!c.item_text_colors.empty()) text_colors.push_back(Rgba8{0, 0, 0, 0});
        }
    }
    if (c.item_colors.empty()) colors.clear();
    c.items = std::move(items);
    c.item_colors = std::move(colors);
    c.item_text_colors = std::move(text_colors);
    if (c.selected >= static_cast<int32_t>(c.items.size())) c.selected = -1;
    return true;
}

bool GuiManager::set_paragraphs(GuiDialog& d, std::string_view control, const std::vector<std::u32string>& paragraphs, Rgba8 color,
                                float max_width) {
    GuiControl* c = d.get(control);
    if (!c) return false;
    if (max_width > 0.0f && c->def && static_cast<float>(c->def->w) > max_width) c->scale.x = max_width / static_cast<float>(c->def->w);
    if (max_width > 0.0f && c->def && c->def->h > 0 && iequals(control, "IDC_LIST_TACTICAL")) {
        c->offset.y = -kPlanetTacticalListDrop;
        c->scale.y = (static_cast<float>(c->def->h) - kPlanetTacticalListDrop) / static_cast<float>(c->def->h);
    }
    std::vector<std::u32string> items;
    for (const std::u32string& para : paragraphs) {  // '\n' splits a paragraph into items
        std::u32string cur;
        for (char32_t ch : para) {
            if (ch == U'\r') continue;
            if (ch == U'\n') {
                items.push_back(std::move(cur));
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        items.push_back(std::move(cur));
    }
    d.set_items(control, std::move(items), -1);
    c->item_text_colors.assign(c->items.size(), color);
    wrap_list_items(d, control);
    c->top = 0;
    return true;
}

bool GuiManager::scroll_list_to(GuiDialog& d, std::string_view control, int32_t row) {
    const int32_t i = d.find(control);
    if (i < 0 || static_cast<size_t>(i) >= d.controls_.size()) return false;
    GuiControl& c = d.controls_[static_cast<size_t>(i)];
    if (c.type != GuiGadgetType::ListBox) return false;
    c.top = row;
    scroll_list(c, static_cast<size_t>(i), d, 0);
    return true;
}

void GuiManager::trim_dialog_height(GuiDialog& d, float units) {
    d.height_trim_ = std::max(0.0f, units);
    place(d);
}

bool GuiManager::scroll_to_end(GuiDialog& d, std::string_view control) {
    const int32_t i = d.find(control);
    if (i < 0 || static_cast<size_t>(i) >= d.controls_.size()) return false;
    GuiControl& c = d.controls_[static_cast<size_t>(i)];
    if (c.type != GuiGadgetType::ListBox) return false;
    c.top = static_cast<int32_t>(c.items.size());
    scroll_list(c, static_cast<size_t>(i), d, 0);
    return true;
}

void GuiManager::list_select(GuiDialog& d, size_t i, int32_t index, std::vector<GuiEvent>& ev) {
    GuiControl& c = d.controls_[i];
    if (c.items.empty()) return;
    index = std::clamp(index, 0, static_cast<int32_t>(c.items.size()) - 1);
    if (c.type == GuiGadgetType::ListBox || c.open) {
        const int32_t vis = scroll_rows(d, i);
        if (index < c.top) c.top = index;
        if (index >= c.top + vis) c.top = index - vis + 1;
    }
    if (index == c.selected) return;
    c.selected = index;
    sfx("GUI_Common_Dialog_Listbox_Entry_Select", static_cast<int32_t>(i));
    ev.push_back(make_event(GuiEvent::Kind::SelectionChanged, d, static_cast<int32_t>(i), index));
}

void GuiManager::slider_set(GuiDialog& d, size_t i, int32_t v, std::vector<GuiEvent>& ev) {
    GuiControl& c = d.controls_[i];
    v = std::clamp(v, c.range_min, c.range_max);
    if (v == c.value) return;
    c.value = v;
    ev.push_back(make_event(GuiEvent::Kind::ValueChanged, d, static_cast<int32_t>(i), v));
}

// ---- Edit boxes ----------------------------------------------------------------------------------------------
// Win32 single-line edit rules [port where noted]: ES_AUTOHSCROLL scrolls the text so the caret stays in view (no
// narrowing); without it typing stops at the field width. A click puts the caret at the nearest character boundary,
// a drag or Shift+click / Shift+arrows select, Ctrl+A selects all, Ctrl+C / X / V (Ctrl+Insert, Shift+Delete,
// Shift+Insert) use the clipboard, typing replaces the selection. Key-capture fields (read_only) keep the one-line
// narrowing: they show a key name the port writes, not typed text.
namespace {
bool autohscroll(const GuiControl& c) { return c.def && c.def->has_style("ES_AUTOHSCROLL"); }
}  // namespace

GuiRect GuiManager::edit_text_rect(const GuiDialog& d, size_t i) const {
    GuiRect r = d.control_rect(i);
    r.min.x += kFieldInset;
    r.max.x -= 2.0f;  // [port] right inset
    return r;
}

float GuiManager::edit_measure(const GuiDialog& d, size_t i, std::u32string_view t) const {
    if (t.empty() || !fonts_ || !skin_) return 0.0f;
    const HudTextStyle s = text_style(d, i, false);
    if (s.font < 0) return 0.0f;
    return measure_text_block(*fonts_, canvas_, t, 0.0f, s).width;
}

size_t GuiManager::edit_index_at(const GuiDialog& d, size_t i, float x) const {
    const GuiControl& c = d[i];
    const GuiRect tr = edit_text_rect(d, i);
    const size_t s0 = std::min(c.scroll_x, c.text.size());
    size_t best = s0;
    float best_d = std::fabs(x - tr.min.x);
    for (size_t k = s0 + 1; k <= c.text.size(); ++k) {
        const float px = tr.min.x + edit_measure(d, i, std::u32string_view(c.text).substr(s0, k - s0));
        const float dist = std::fabs(x - px);
        if (dist < best_d) {
            best = k;
            best_d = dist;
        }
        if (px > tr.max.x) break;
    }
    return best;
}

void GuiManager::edit_keep_caret_visible(GuiDialog& d, size_t i) const {
    GuiControl& c = d.controls_[i];
    c.caret = std::min(c.caret, c.text.size());
    c.sel_anchor = std::min(c.sel_anchor, c.text.size());
    if (!autohscroll(c) || c.read_only) {
        c.scroll_x = 0;
        return;
    }
    const float avail = edit_text_rect(d, i).width();
    const std::u32string_view t = c.text;
    c.scroll_x = std::min(c.scroll_x, c.caret);
    while (c.scroll_x < c.caret && edit_measure(d, i, t.substr(c.scroll_x, c.caret - c.scroll_x)) > avail) ++c.scroll_x;
    while (c.scroll_x > 0 && edit_measure(d, i, t.substr(c.scroll_x - 1)) <= avail) --c.scroll_x;  // room on the right
}

bool GuiManager::edit_insert(GuiDialog& d, size_t i, std::u32string_view t) const {
    GuiControl& c = d.controls_[i];
    bool changed = false;
    const float avail = edit_text_rect(d, i).width();
    for (char32_t ch : t) {
        if (ch < 0x20 || ch == 0x7F || c.text.size() >= c.max_chars) continue;
        if (!autohscroll(c)) {  // Win32: without ES_AUTOHSCROLL the text may not grow past the field
            std::u32string test = c.text;
            test.insert(c.caret, 1, ch);
            if (edit_measure(d, i, test) > avail) break;
        }
        c.text.insert(c.caret, 1, ch);
        ++c.caret;
        changed = true;
    }
    c.sel_anchor = c.caret;
    return changed;
}

void GuiManager::click(GuiDialog& d, size_t i, std::vector<GuiEvent>& ev, bool keyboard) {
    GuiControl& c = d.controls_[i];
    if (!c.enabled || !c.visible) return;
    (void)keyboard;
    switch (c.type) {
        case GuiGadgetType::PushButton:
            sfx("GUI_Common_Dialog_Button_Press", static_cast<int32_t>(i));
            ev.push_back(make_event(GuiEvent::Kind::Clicked, d, static_cast<int32_t>(i)));
            break;
        case GuiGadgetType::CheckBox:
            c.checked = !c.checked;
            sfx("GUI_Common_Dialog_Button_Press", static_cast<int32_t>(i));
            ev.push_back(make_event(GuiEvent::Kind::Checked, d, static_cast<int32_t>(i), c.checked ? 1 : 0));
            break;
        case GuiGadgetType::RadioButton:
            sfx("GUI_Common_Dialog_Button_Press", static_cast<int32_t>(i));
            if (!c.checked) {
                d.set_checked(c.id(), true);
                ev.push_back(make_event(GuiEvent::Kind::Checked, d, static_cast<int32_t>(i), 1));
            }
            break;
        default: break;
    }
}

void GuiManager::move_focus(GuiDialog& d, int32_t dir) {
    const auto n = static_cast<int32_t>(d.size());
    if (n == 0) return;
    int32_t i = d.focus_;
    for (int32_t k = 0; k < n; ++k) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (focusable(d[static_cast<size_t>(i)])) {
            d.focus_ = i;
            d.keyboard_focus_ = true;
            return;
        }
    }
}

void GuiManager::key_input(GuiDialog& d, const InputFrame& raw, std::u32string_view typed, std::vector<GuiEvent>& ev) {
    const KeySet& k = raw.keys_pressed;
    const bool shift = raw.keys_down.test(vk::kShift);
    GuiControl* f = d.focus_ >= 0 && static_cast<size_t>(d.focus_) < d.size() ? &d.controls_[static_cast<size_t>(d.focus_)]
                                                                            : nullptr;
    if (f && !focusable(*f)) f = nullptr;
    const auto fi = static_cast<size_t>(std::max(d.focus_, 0));
    const bool edit = f && (f->type == GuiGadgetType::EditBox || f->type == GuiGadgetType::IMEEditBox);

    if (k.test(vk::kTab) && !(edit && f->key_capture)) move_focus(d, shift ? -1 : 1);
    if (k.test(vk::kEscape)) {
        bool closed = false;
        for (GuiControl& c : d.controls_)
            if (c.open) {
                c.open = false;
                closed = true;
            }
        if (!closed) ev.push_back(make_event(GuiEvent::Kind::Cancel, d, -1));
    }
    if (k.test(vk::kReturn) && !(edit && f->read_only)) {  // key-capture fields take Enter as a key
        if (edit) {
            GuiEvent e = make_event(GuiEvent::Kind::Submitted, d, d.focus_);
            e.text = f->text;
            ev.push_back(std::move(e));
        } else if (f && f->type == GuiGadgetType::ComboBox && f->open) {
            f->open = false;
        } else if (f && f->type == GuiGadgetType::PushButton) {
            click(d, fi, ev, true);
        } else {
            for (size_t i = 0; i < d.size(); ++i)
                if (d[i].def->default_button && d[i].visible && d[i].enabled) {
                    click(d, i, ev, true);
                    break;
                }
        }
    }
    if (!f) return;
    if (edit && f->read_only) return;
    if (edit) {
        const bool ctrl = raw.keys_down.test(vk::kControl);
        GuiControl& e = *f;
        e.caret = std::min(e.caret, e.text.size());
        e.sel_anchor = std::min(e.sel_anchor, e.text.size());
        auto lo = [&] { return std::min(e.caret, e.sel_anchor); };
        auto hi = [&] { return std::max(e.caret, e.sel_anchor); };
        auto copy_sel = [&] {
            if (lo() == hi()) return;
            std::u32string t = e.text.substr(lo(), hi() - lo());
            if (clip_set_) clip_set_(t);
            else clipboard_ = std::move(t);
        };
        auto erase_sel = [&] {
            if (lo() == hi()) return false;
            const size_t a = lo();
            e.text.erase(a, hi() - a);
            e.caret = e.sel_anchor = a;
            return true;
        };
        auto word_left = [&](size_t p) {
            while (p > 0 && e.text[p - 1] == U' ') --p;
            while (p > 0 && e.text[p - 1] != U' ') --p;
            return p;
        };
        auto word_right = [&](size_t p) {
            while (p < e.text.size() && e.text[p] != U' ') ++p;
            while (p < e.text.size() && e.text[p] == U' ') ++p;
            return p;
        };
        auto move = [&](size_t to) {
            e.caret = to;
            if (!shift) e.sel_anchor = to;
        };
        bool changed = false;
        if (ctrl && k.test(vk::key('a'))) {
            e.sel_anchor = 0;
            e.caret = e.text.size();
        }
        if (ctrl && (k.test(vk::key('c')) || k.test(vk::kInsert))) copy_sel();
        const bool cut = (ctrl && k.test(vk::key('x'))) || (shift && !ctrl && k.test(vk::kDelete));
        if (cut) {
            copy_sel();
            if (erase_sel()) changed = true;
        }
        if ((ctrl && k.test(vk::key('v'))) || (shift && !ctrl && k.test(vk::kInsert))) {
            std::u32string clip = clip_get_ ? clip_get_() : clipboard_;
            for (char32_t& ch : clip)
                if (ch == U'\n' || ch == U'\r' || ch == U'\t') ch = U' ';  // one-line field
            if (erase_sel()) changed = true;
            if (edit_insert(d, fi, clip)) changed = true;
        }
        if (k.test(vk::kBack)) {
            if (erase_sel()) {
                changed = true;
            } else if (e.caret > 0) {
                e.text.erase(e.caret - 1, 1);
                e.caret = e.sel_anchor = e.caret - 1;
                changed = true;
            }
        }
        if (k.test(vk::kDelete) && !cut) {
            if (erase_sel()) {
                changed = true;
            } else if (e.caret < e.text.size()) {
                e.text.erase(e.caret, 1);
                changed = true;
            }
        }
        if (k.test(vk::kLeft)) {
            if (!shift && !ctrl && lo() != hi()) move(lo());
            else move(e.caret == 0 ? 0 : ctrl ? word_left(e.caret) : e.caret - 1);
        }
        if (k.test(vk::kRight)) {
            if (!shift && !ctrl && lo() != hi()) move(hi());
            else move(e.caret >= e.text.size() ? e.text.size() : ctrl ? word_right(e.caret) : e.caret + 1);
        }
        if (k.test(vk::kHome)) move(0);
        if (k.test(vk::kEnd)) move(e.text.size());
        std::u32string add(typed);
        if (typed.empty() && !ctrl) {  // [port] VK fallback when the host has no WM_CHAR text
            for (uint32_t c = 'A'; c <= 'Z'; ++c)
                if (k.test(c)) add.push_back(shift ? static_cast<char32_t>(c) : static_cast<char32_t>(c - 'A' + 'a'));
            for (uint32_t c = '0'; c <= '9'; ++c)
                if (k.test(c) && !shift) add.push_back(static_cast<char32_t>(c));
            if (k.test(vk::kSpace)) add.push_back(U' ');
            if (k.test(0xBD)) add.push_back(shift ? U'_' : U'-');
            if (k.test(0xBE)) add.push_back(U'.');
        }
        bool printable = false;
        for (char32_t c : add) printable = printable || (c >= 0x20 && c != 0x7F);
        if (printable) {
            if (erase_sel()) changed = true;
            if (edit_insert(d, fi, add)) changed = true;
        }
        edit_keep_caret_visible(d, fi);
        if (changed) {
            GuiEvent te = make_event(GuiEvent::Kind::TextChanged, d, d.focus_);
            te.text = f->text;
            ev.push_back(std::move(te));
        }
        return;
    }
    if (k.test(vk::kSpace) && clickable(f->type)) click(d, fi, ev, true);
    switch (f->type) {
        case GuiGadgetType::ListBox:
        case GuiGadgetType::ComboBox: {
            int32_t step = 0;
            if (k.test(vk::kUp)) step = -1;
            if (k.test(vk::kDown)) step = 1;
            if (f->type == GuiGadgetType::ListBox || f->open) {
                if (k.test(vk::kPageUp)) step = -scroll_rows(d, fi);
                if (k.test(vk::kPageDown)) step = scroll_rows(d, fi);
            }
            if (step != 0) list_select(d, fi, f->selected < 0 ? 0 : f->selected + step, ev);
            if (k.test(vk::kHome)) list_select(d, fi, 0, ev);
            if (k.test(vk::kEnd)) list_select(d, fi, static_cast<int32_t>(f->items.size()) - 1, ev);
            break;
        }
        case GuiGadgetType::RadioButton: {
            int32_t dir = 0;
            if (k.test(vk::kUp) || k.test(vk::kLeft)) dir = -1;
            if (k.test(vk::kDown) || k.test(vk::kRight)) dir = 1;
            if (dir == 0) break;
            const auto n = static_cast<int32_t>(d.size());
            for (int32_t s = 1; s < n; ++s) {
                const auto j = static_cast<size_t>((d.focus_ + dir * s + n) % n);
                GuiControl& o = d.controls_[j];
                if (o.type == GuiGadgetType::RadioButton && o.radio_group == f->radio_group && o.visible && o.enabled) {
                    d.focus_ = static_cast<int32_t>(j);
                    click(d, j, ev, true);
                    break;
                }
            }
            break;
        }
        case GuiGadgetType::TrackbarSlider: {
            const int32_t page = std::max(1, (f->range_max - f->range_min) / 5);
            int32_t v = f->value;
            if (k.test(vk::kLeft) || k.test(vk::kUp)) v -= 1;
            if (k.test(vk::kRight) || k.test(vk::kDown)) v += 1;
            if (k.test(vk::kPageUp)) v -= page;
            if (k.test(vk::kPageDown)) v += page;
            if (k.test(vk::kHome)) v = f->range_min;
            if (k.test(vk::kEnd)) v = f->range_max;
            slider_set(d, fi, v, ev);
            break;
        }
        default: break;
    }
}

void GuiManager::update(const InputFrame& raw, const RoutedInput& routed, double now, std::vector<GuiEvent>& events,
                        std::u32string_view typed) {
    cursor_ = canvas_.pixel_to_canvas(static_cast<float>(raw.mouse_x) + 0.5f, static_cast<float>(raw.mouse_y) + 0.5f);
    for (auto& dp : stack_)  // closed combo hit height follows the skin (the image resolver may load it late)
        for (size_t i = 0; i < dp->size(); ++i)
            if (dp->controls_[i].type == GuiGadgetType::ComboBox) dp->controls_[i].field_height = combo_field_height(*dp, i);
    // Pointer target: the topmost dialog (inside it, or anywhere when modal), else an always-input dialog under it.
    GuiDialog* target = nullptr;
    if (!stack_.empty()) {
        GuiDialog& t = *stack_.back();
        bool in_popup = false;
        for (size_t i = 0; i < t.size(); ++i)
            if (t[i].open && combo_list(t, i).contains(cursor_)) in_popup = true;
        if (t.opt_.modal || t.rect().contains(cursor_) || in_popup) target = &t;
        if (!target)
            for (size_t i = stack_.size(); i-- > 0;)
                if (stack_[i]->opt_.always_input && stack_[i]->rect().contains(cursor_)) {
                    target = stack_[i].get();
                    break;
                }
    }
    // Hover.
    const int32_t hc = target && raw.mouse_inside ? target->control_at(cursor_) : -1;
    if (target != hover_dialog_ || hc != hover_control_) {
        hover_dialog_ = target;
        hover_control_ = hc;
        hover_since_ = now;
        if (target && hc >= 0) {
            const GuiControl& c = (*target)[static_cast<size_t>(hc)];
            if (clickable(c.type) && c.enabled) sfx("GUI_Common_Dialog_Button_Hilight", hc);
        }
    }

    const uint8_t lb = button_bit(Button::Left);
    const bool dialog_owns_left = routed.owner[static_cast<size_t>(Button::Left)] == PointerTarget::Dialog;
    const bool pressed = (raw.buttons_pressed & lb) != 0 && (dialog_owns_left || target != nullptr);
    const bool released = (raw.buttons_released & lb) != 0 || ((raw.buttons_down & lb) == 0 && press_dialog_ != nullptr);

    auto slider_from_cursor = [&](GuiDialog& d, size_t i) {
        const GuiControl& c = d[i];
        const GuiRect r = d.control_rect(i);
        const float t = r.width() > 0 ? std::clamp((cursor_.x - r.min.x) / r.width(), 0.0f, 1.0f) : 0.0f;
        const int32_t v = c.range_min + static_cast<int32_t>(std::lround(t * static_cast<float>(c.range_max - c.range_min)));
        slider_set(d, i, v, events);
    };

    if (pressed && target) {
        bool consumed = false;
        // An open combo drop-down takes the press first (select a row, or close).
        for (size_t i = 0; i < target->size() && !consumed; ++i) {
            GuiControl& c = target->controls_[i];
            if (!c.open) continue;
            consumed = true;
            const ScrollBar sb = scroll_bar(*target, i);
            if (const ScrollPart part = scroll_part_at(sb, cursor_); part != ScrollPart::None) {
                scroll_press(*target, i, part, now);  // the drop-down's scroll bar: stays open
                continue;
            }
            const GuiRect lr = combo_list(*target, i);
            if (lr.contains(cursor_)) {
                const float rh = row_height(*target, i);
                const int32_t row = c.top + static_cast<int32_t>((lr.max.y - 2.0f - cursor_.y) / rh);
                if (row >= c.top && row < c.top + sb.vis && row < static_cast<int32_t>(c.items.size()))
                    list_select(*target, i, row, events);
            }
            c.open = false;
        }
        if (!consumed && hc >= 0) {
            const auto i = static_cast<size_t>(hc);
            GuiControl& c = target->controls_[i];
            if (c.enabled) {
                press_dialog_ = target;
                press_control_ = hc;
                if (focusable(c)) {
                    target->focus_ = hc;
                    target->keyboard_focus_ = false;
                }
                const GuiRect r = target->control_rect(i);
                switch (c.type) {
                    case GuiGadgetType::TrackbarSlider:
                        slider_drag_ = true;
                        slider_from_cursor(*target, i);
                        break;
                    case GuiGadgetType::ComboBox:
                        c.open = !c.open && !c.items.empty();
                        if (c.open) {  // the drop-down opens scrolled to the selection (first row when it can be)
                            const int32_t max_top = std::max(0, static_cast<int32_t>(c.items.size()) - combo_rows(*target, i));
                            c.top = std::clamp(c.selected, 0, max_top);
                        }
                        break;
                    case GuiGadgetType::EditBox:
                    case GuiGadgetType::IMEEditBox:
                        c.caret = edit_index_at(*target, i, cursor_.x);
                        if (!raw.keys_down.test(vk::kShift)) c.sel_anchor = c.caret;
                        edit_drag_ = true;
                        edit_keep_caret_visible(*target, i);
                        break;
                    case GuiGadgetType::ListBox: {
                        const ScrollBar sb = scroll_bar(*target, i);
                        if (const ScrollPart part = scroll_part_at(sb, cursor_); part != ScrollPart::None) {
                            scroll_press(*target, i, part, now);
                        } else if (sb.active && cursor_.x >= sb.col.min.x) {
                            // inside the column but outside every part: nothing
                        } else {
                            const float rh = row_height(*target, i);
                            const int32_t row = c.top + static_cast<int32_t>((r.max.y - 2.0f - cursor_.y) / rh);
                            if (row >= 0 && row < static_cast<int32_t>(c.items.size())) {
                                const bool dbl = last_list_click_ == hc && last_list_row_ == row &&
                                                 now - last_list_time_ <= style_.double_click;
                                list_select(*target, i, row, events);
                                if (dbl) {
                                    events.push_back(make_event(GuiEvent::Kind::DoubleClicked, *target, hc, row));
                                    last_list_time_ = -1e30;
                                } else {
                                    last_list_click_ = hc;
                                    last_list_row_ = row;
                                    last_list_time_ = now;
                                }
                            }
                        }
                        break;
                    }
                    default: break;
                }
            }
        }
    }
    if (slider_drag_ && press_dialog_ && press_control_ >= 0 && (raw.buttons_down & lb) != 0)
        slider_from_cursor(*press_dialog_, static_cast<size_t>(press_control_));
    if (scroll_part_ != ScrollPart::None && (raw.buttons_down & lb) != 0) scroll_held(now);
    if (edit_drag_ && press_dialog_ && press_control_ >= 0 && (raw.buttons_down & lb) != 0 && !pressed) {
        const auto i = static_cast<size_t>(press_control_);
        GuiControl& c = press_dialog_->controls_[i];
        if (c.type == GuiGadgetType::EditBox || c.type == GuiGadgetType::IMEEditBox) {
            const GuiRect tr = edit_text_rect(*press_dialog_, i);
            if (cursor_.x < tr.min.x && c.scroll_x > 0) c.caret = c.scroll_x - 1;  // drag past the left edge scrolls
            else c.caret = edit_index_at(*press_dialog_, i, cursor_.x);
            edit_keep_caret_visible(*press_dialog_, i);
        }
    }
    if (released && press_dialog_) {
        if (press_dialog_ == hover_dialog_ && press_control_ == hover_control_ && press_control_ >= 0 && !slider_drag_ &&
            scroll_part_ == ScrollPart::None)
            click(*press_dialog_, static_cast<size_t>(press_control_), events, false);
        press_dialog_ = nullptr;
        press_control_ = -1;
        slider_drag_ = false;
    }
    if (released || (raw.buttons_down & lb) == 0) {
        scroll_part_ = ScrollPart::None;
        edit_drag_ = false;
    }

    // Wheel: dialogs only while they have focus (InputRouter's rule): scroll the list / open combo under the cursor.
    bool wheel_used = false;
    if (raw.wheel != 0.0f && routed.wheel_target == PointerTarget::Dialog && target) {  // an open drop-down first
        const auto notches = static_cast<int32_t>(std::lround(raw.wheel));
        for (size_t i = 0; i < target->size() && !wheel_used; ++i) {
            GuiControl& c = target->controls_[i];
            if (c.open && combo_list(*target, i).contains(cursor_)) {
                scroll_list(c, i, *target, -3 * notches);  // [port] 3 rows / notch
                wheel_used = true;
            }
        }
    }
    if (!wheel_used && raw.wheel != 0.0f && routed.wheel_target == PointerTarget::Dialog && target && hc >= 0) {
        const auto i = static_cast<size_t>(hc);
        GuiControl& c = target->controls_[i];
        const auto notches = static_cast<int32_t>(std::lround(raw.wheel));
        if (c.type == GuiGadgetType::ListBox) scroll_list(c, i, *target, -3 * notches);  // [port] 3 rows / notch
        if (c.type == GuiGadgetType::TrackbarSlider) slider_set(*target, i, c.value + notches, events);
    }

    // Keys / typed text: the topmost dialog while one has focus.
    if (has_focus() && !stack_.empty()) key_input(*stack_.back(), raw, typed, events);
}

// ---- Drawing ------------------------------------------------------------------------------------------------
GuiImage GuiManager::image(std::string_view texture) const {
    GuiImage img;
    if (texture.empty()) return img;
    if (atlas_)
        if (const AtlasEntry* e = atlas_->find(texture)) {
            const auto tw = static_cast<float>(std::max(1u, atlas_->texture_width));
            const auto th = static_cast<float>(std::max(1u, atlas_->texture_height));
            img.valid = true;
            img.page = kHudPageAtlas;
            img.uv0 = {static_cast<float>(e->x) / tw, static_cast<float>(e->y) / th};
            img.uv1 = {static_cast<float>(e->x + e->width) / tw, static_cast<float>(e->y + e->height) / th};
            img.size = {static_cast<float>(e->width), static_cast<float>(e->height)};
            return img;
        }
    if (images_) return images_(texture);
    return img;
}

const GuiFontSpec& GuiManager::font_spec(const GuiDialog& d, size_t i) const {
    const GuiControl& c = d[i];
    if (c.ui_font) return skin_->font(d.name(), {}, GuiGadgetType::Text, TextAlign::Left);
    return skin_->font(d.name(), c.id(), c.type, c.def->align);
}

int32_t GuiManager::control_font(const GuiDialog& d, size_t i) const {
    if (!fonts_ || !skin_) return -1;
    const GuiFontSpec& f = font_spec(d, i);
    return fonts_->font(f.face, f.size, canvas_.screen_height, f.stretch);
}

HudTextStyle GuiManager::text_style(const GuiDialog& d, size_t i, bool disabled) const {
    HudTextStyle s;
    const GuiControl& c = d[i];
    const GuiFontSpec& f = font_spec(d, i);
    s.font = fonts_->font(f.face, f.size, canvas_.screen_height, f.stretch);
    s.color = disabled ? style_.disabled_text : f.top;
    s.color_bottom = f.bottom;
    s.gradient = !disabled && !(f.top == f.bottom);
    s.emboss = f.emboss;
    s.outline = f.outline;
    s.char_padding_px = f.padding;
    if (c.text_outline) s.outline = true;
    if (c.tint.a != 255 || c.tint.r != 255 || c.tint.g != 255 || c.tint.b != 255) {  // GUI_Text_Color style override
        s.color = c.tint;
        s.gradient = false;
    }
    return s;
}

bool GuiManager::quad(HudDrawList& dl, uint32_t layer, std::string_view tex, GuiRect r, Rgba8 color) const {
    const GuiImage img = image(tex);
    if (!img.valid || r.width() <= 0 || r.height() <= 0) return false;
    HudQuad q;
    q.min = r.min;
    q.max = r.max;
    q.uv0 = img.uv0;
    q.uv1 = img.uv1;
    q.color = color;
    q.page = img.page;
    dl.add(layer, HudList::Alpha, q);
    return true;
}

void GuiManager::fill(HudDrawList& dl, uint32_t layer, GuiRect r, Rgba8 color) const {
    if (r.width() <= 0 || r.height() <= 0) return;
    HudQuad q;
    q.min = r.min;
    q.max = r.max;
    q.color = color;
    q.page = kHudPageWhite;
    dl.add(layer, HudList::Alpha, q);
}

void GuiManager::frame16(HudDrawList& dl, uint32_t layer, const GuiDialog& d) const {
    const std::string_view dn = d.name();
    auto tex = [&](std::string_view key) { return skin_->texture(dn, {}, key); };
    auto size = [&](std::string_view key) {
        const GuiImage im = image(tex(key));
        return im.valid ? im.size : Vec2f{};
    };
    const GuiRect r = d.rect();
    const std::string_view bg = tex("Frame_Background");
    if (!bg.empty() && gui_full_screen_art(image(bg))) {  // 1:1 on the centred base area, no frame pieces
        quad(dl, layer, bg, {{-0.5f * kHudBaseWidth, -0.5f * kHudBaseHeight}, {0.5f * kHudBaseWidth, 0.5f * kHudBaseHeight}});
        return;
    }
    if (!bg.empty() && !quad(dl, layer, bg, r) && style_.fallback_art) fill(dl, layer, r, style_.fallback_background);
    const Vec2f tl = size("Frame_Top_Left"), tr = size("Frame_Top_Right"), br = size("Frame_Bottom_Right"),
                bl = size("Frame_Bottom_Left");
    quad(dl, layer, tex("Frame_Top_Left"), {{r.min.x, r.max.y - tl.y}, {r.min.x + tl.x, r.max.y}});
    quad(dl, layer, tex("Frame_Top_Right"), {{r.max.x - tr.x, r.max.y - tr.y}, {r.max.x, r.max.y}});
    quad(dl, layer, tex("Frame_Bottom_Right"), {{r.max.x - br.x, r.min.y}, {r.max.x, r.min.y + br.y}});
    quad(dl, layer, tex("Frame_Bottom_Left"), {{r.min.x, r.min.y}, {r.min.x + bl.x, r.min.y + bl.y}});
    // Top edge: transition pieces next to the corners, the edge stretched between [port].
    {
        const Vec2f a = size("Frame_Top_Transition_Left"), b = size("Frame_Top_Transition_Right"), m = size("Frame_Top");
        const float x0 = r.min.x + tl.x, x1 = r.max.x - tr.x;
        quad(dl, layer, tex("Frame_Top_Transition_Left"), {{x0, r.max.y - a.y}, {x0 + a.x, r.max.y}});
        quad(dl, layer, tex("Frame_Top_Transition_Right"), {{x1 - b.x, r.max.y - b.y}, {x1, r.max.y}});
        quad(dl, layer, tex("Frame_Top"), {{x0 + a.x, r.max.y - m.y}, {x1 - b.x, r.max.y}});
    }
    {
        const Vec2f a = size("Frame_Bottom_Transition_Left"), b = size("Frame_Bottom_Transition_Right"),
                    m = size("Frame_Bottom");
        const float x0 = r.min.x + bl.x, x1 = r.max.x - br.x;
        quad(dl, layer, tex("Frame_Bottom_Transition_Left"), {{x0, r.min.y}, {x0 + a.x, r.min.y + a.y}});
        quad(dl, layer, tex("Frame_Bottom_Transition_Right"), {{x1 - b.x, r.min.y}, {x1, r.min.y + b.y}});
        quad(dl, layer, tex("Frame_Bottom"), {{x0 + a.x, r.min.y}, {x1 - b.x, r.min.y + m.y}});
    }
    {
        const Vec2f a = size("Frame_Left_Transition_Top"), b = size("Frame_Left_Transition_Bottom"), m = size("Frame_Left");
        const float y1 = r.max.y - tl.y, y0 = r.min.y + bl.y;
        quad(dl, layer, tex("Frame_Left_Transition_Top"), {{r.min.x, y1 - a.y}, {r.min.x + a.x, y1}});
        quad(dl, layer, tex("Frame_Left_Transition_Bottom"), {{r.min.x, y0}, {r.min.x + b.x, y0 + b.y}});
        quad(dl, layer, tex("Frame_Left"), {{r.min.x, y0 + b.y}, {r.min.x + m.x, y1 - a.y}});
    }
    {
        const Vec2f a = size("Frame_Right_Transition_Top"), b = size("Frame_Right_Transition_Bottom"),
                    m = size("Frame_Right");
        const float y1 = r.max.y - tr.y, y0 = r.min.y + br.y;
        quad(dl, layer, tex("Frame_Right_Transition_Top"), {{r.max.x - a.x, y1 - a.y}, {r.max.x, y1}});
        quad(dl, layer, tex("Frame_Right_Transition_Bottom"), {{r.max.x - b.x, y0}, {r.max.x, y0 + b.y}});
        quad(dl, layer, tex("Frame_Right"), {{r.max.x - m.x, y0 + b.y}, {r.max.x, y1 - a.y}});
    }
}

void GuiManager::frame9(HudDrawList& dl, uint32_t layer, const GuiDialog& d, std::string_view control, GuiRect r,
                        bool background) const {
    const std::string_view dn = d.name();
    auto tex = [&](std::string_view key) { return skin_->texture(dn, control, key); };
    auto size = [&](std::string_view key) {
        const GuiImage im = image(tex(key));
        return im.valid ? im.size : Vec2f{};
    };
    if (background) {
        const std::string_view bg = tex("Small_Frame_Background");
        if (!quad(dl, layer, bg, r) && style_.fallback_art) fill(dl, layer, r, {10, 12, 18, 200});
    }
    const Vec2f tl = size("Small_Frame_Top_Left"), tr = size("Small_Frame_Top_Right"), br = size("Small_Frame_Bottom_Right"),
                bl = size("Small_Frame_Bottom_Left"), t = size("Small_Frame_Top"), b = size("Small_Frame_Bottom"),
                l = size("Small_Frame_Left"), rr = size("Small_Frame_Right");
    quad(dl, layer, tex("Small_Frame_Top_Left"), {{r.min.x, r.max.y - tl.y}, {r.min.x + tl.x, r.max.y}});
    quad(dl, layer, tex("Small_Frame_Top_Right"), {{r.max.x - tr.x, r.max.y - tr.y}, {r.max.x, r.max.y}});
    quad(dl, layer, tex("Small_Frame_Bottom_Right"), {{r.max.x - br.x, r.min.y}, {r.max.x, r.min.y + br.y}});
    quad(dl, layer, tex("Small_Frame_Bottom_Left"), {{r.min.x, r.min.y}, {r.min.x + bl.x, r.min.y + bl.y}});
    quad(dl, layer, tex("Small_Frame_Top"), {{r.min.x + tl.x, r.max.y - t.y}, {r.max.x - tr.x, r.max.y}});
    quad(dl, layer, tex("Small_Frame_Bottom"), {{r.min.x + bl.x, r.min.y}, {r.max.x - br.x, r.min.y + b.y}});
    quad(dl, layer, tex("Small_Frame_Left"), {{r.min.x, r.min.y + bl.y}, {r.min.x + l.x, r.max.y - tl.y}});
    quad(dl, layer, tex("Small_Frame_Right"), {{r.max.x - rr.x, r.min.y + br.y}, {r.max.x, r.max.y - tr.y}});
}

void GuiManager::three_piece(HudDrawList& dl, uint32_t layer, const GuiDialog& d, std::string_view control,
                             std::string_view left, std::string_view middle, std::string_view right, GuiRect r,
                             float fraction) const {
    const std::string_view dn = d.name();
    const GuiImage li = image(skin_->texture(dn, control, left)), ri = image(skin_->texture(dn, control, right));
    // [port] a stand-alone (non-atlas) middle texture whose size is not a power of two covers the rect grown to the
    // next power of two from its top-left corner, as the original's D3D upload rescales such textures. Evidence:
    // IDC_MENU_PETRO_LOGO (118 x 118 control, i_button_petro.dds 118 x 118) matches the logo painted in
    // MenuBack_overlay only at 128 x 128 (mean error 8 vs 61 at 118; the 118 quad showed the wordmark twice).
    if (const GuiImage mi = image(skin_->texture(dn, control, middle)); mi.valid && mi.page >= kHudPageImageBase) {
        auto grow = [](float s) {
            if (s <= 0.0f) return 1.0f;
            float p = 1.0f;
            while (p < s) p *= 2.0f;
            return p / s;
        };
        const float gx = grow(mi.size.x), gy = grow(mi.size.y);
        r.max.x = r.min.x + r.width() * gx;
        r.min.y = r.max.y - r.height() * gy;
    }
    const float h = r.height();
    float lw = li.valid && li.size.y > 0 ? li.size.x * h / li.size.y : 0.0f;
    float rw = ri.valid && ri.size.y > 0 ? ri.size.x * h / ri.size.y : 0.0f;
    if (lw + rw > r.width() && lw + rw > 0) {
        const float s = r.width() / (lw + rw);
        lw *= s;
        rw *= s;
    }
    quad(dl, layer, skin_->texture(dn, control, left), {r.min, {r.min.x + lw, r.max.y}});
    quad(dl, layer, skin_->texture(dn, control, right), {{r.max.x - rw, r.min.y}, r.max});
    const float x0 = r.min.x + lw, x1 = r.max.x - rw;
    const float xm = x0 + (x1 - x0) * std::clamp(fraction, 0.0f, 1.0f);
    if (xm > x0) quad(dl, layer, skin_->texture(dn, control, middle), {{x0, r.min.y}, {xm, r.max.y}});
}

void GuiManager::draw_text(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, std::u32string_view text,
                           GuiRect r, TextAlign align, bool wrap, bool vcenter, bool disabled, const Rgba8* color) const {
    if (text.empty() || !fonts_) return;
    HudTextStyle s = text_style(d, i, disabled);
    if (s.font < 0) return;
    if (color && !disabled) {  // [port] explicit colour (story TEXTCOLOR): flat, no font gradient
        s.color = *color;
        s.gradient = false;
    }
    s.align = align;
    // [port] one-line captions wider than their control (the substituted system faces are wider than the EmpireAtWar
    // TTFs the layouts were made for) are narrowed, at most to half, so they stop at the control edge instead of running
    // into the next control or out of the dialog. t47: by width_scale; Stretch_Factor changes only the glyph height
    // (GALACTIC_DIALOGS §6), so stretched fonts came out squat and just as wide.
    if (!wrap && r.width() > 0.0f && text.find(U'\n') == std::u32string_view::npos)
        s.width_scale = fit_width_scale(measure_text_block(*fonts_, canvas_, text, 0.0f, s).width, r.width());
    const float ax = align == TextAlign::Left ? r.min.x : align == TextAlign::Right ? r.max.x : 0.5f * (r.min.x + r.max.x);
    const float max_w = wrap ? r.width() : 0.0f;
    float top = r.max.y;
    if (vcenter) {
        const TextBlock b = measure_text_block(*fonts_, canvas_, text, max_w, s);
        top = 0.5f * (r.min.y + r.max.y) + 0.5f * b.height;
    }
    add_text_block(dl, layer, HudList::Text, *fonts_, canvas_, text, {ax, top}, max_w, s);
}

void GuiManager::draw_fit_text(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, GuiRect r) const {
    const GuiControl& c = d[i];
    if (c.text.empty() || !fonts_) return;
    HudTextStyle s = text_style(d, i, !c.enabled);
    if (s.font < 0) return;
    s.align = c.def->align;
    std::u32string block;
    for (const std::u32string& line : fit_text_lines(*fonts_, s.font, c.text, wrap_width_px(canvas_, r.width(), 1.0f), c.fit_lines, c.fit_keep_tail)) {
        if (!block.empty()) block.push_back(U'\n');
        block += line;
    }
    const float ax = s.align == TextAlign::Left ? r.min.x : s.align == TextAlign::Right ? r.max.x : 0.5f * (r.min.x + r.max.x);
    const TextBlock b = measure_text_block(*fonts_, canvas_, block, 0.0f, s);
    add_text_block(dl, layer, HudList::Text, *fonts_, canvas_, block, {ax, 0.5f * (r.min.y + r.max.y) + 0.5f * b.height}, 0.0f, s);
}

void GuiManager::draw_scroll_bar(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, const ScrollBar& sb) const {
    const std::string_view dn = d.name(), id = d[i].id();
    const bool held = press_dialog_ == &d && press_control_ == static_cast<int32_t>(i);
    // Arrow art: _Pressed while that arrow is held and the cursor is on it, _Mouse_Over under the cursor (skin keys
    // Scroll_Up_Button / Scroll_Down_Button [_Pressed / _Mouse_Over], GUIDialogs.xml <Default>).
    auto arrow = [&](std::string_view base, const GuiRect& r, ScrollPart part) {
        std::string k(base);
        if (r.contains(cursor_)) k += held && scroll_part_ == part ? "_Pressed" : "_Mouse_Over";
        if (k.size() != base.size() && !image(skin_->texture(dn, id, k)).valid) k = std::string(base);
        if (!quad(dl, layer, skin_->texture(dn, id, k), r) && style_.fallback_art)
            fill(dl, layer, r, r.contains(cursor_) ? style_.fallback_button_hot : style_.fallback_button);
    };
    if (!quad(dl, layer, skin_->texture(dn, id, "Scroll_Middle"), sb.col) && style_.fallback_art)
        fill(dl, layer, sb.col, {40, 44, 55, 200});
    arrow("Scroll_Up_Button", sb.up, ScrollPart::Up);
    arrow("Scroll_Down_Button", sb.down, ScrollPart::Down);
    if (!quad(dl, layer, skin_->texture(dn, id, "Scroll_Tab"), sb.thumb) && style_.fallback_art)
        fill(dl, layer, sb.thumb, {120, 130, 150, 255});
}

void GuiManager::draw_control(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, double now) const {
    const GuiControl& c = d[i];
    if (!c.visible) return;
    const std::string_view dn = d.name(), id = c.id();
    const GuiRect r = d.control_rect(i);
    const bool hot = hover_dialog_ == &d && hover_control_ == static_cast<int32_t>(i);
    const bool down = hot && press_dialog_ == &d && press_control_ == static_cast<int32_t>(i);
    const bool focused = d.focus_ == static_cast<int32_t>(i) && d.keyboard_focus_;
    auto state_suffix = [&](bool pressed_look) -> std::string_view {
        if (!c.enabled) return "_Disabled";
        if (down || pressed_look) return "_Pressed";
        if (hot || focused) return "_Mouse_Over";
        return "";
    };
    switch (c.type) {
        case GuiGadgetType::PushButton:
        case GuiGadgetType::RadioButton:
        case GuiGadgetType::CheckBox: {
            const bool pushlike = c.type == GuiGadgetType::PushButton || c.def->pushlike;
            // [port: Gabriel playtest] a push-like radio / check box with its own Radio_On / _Off / _Mouse_Over skin (the
            // campaign difficulty buttons: i_button_difficulty*.tga, one 175 x 23 piece each) draws that one piece over
            // the rect - checked = Radio_On, hovered = Radio_Mouse_Over, else Radio_Off - instead of the generic
            // three-piece pressed button (a glaring white bar).
            if (pushlike && c.type != GuiGadgetType::PushButton) {
                const std::string_view on = skin_->texture(dn, id, "Radio_On");
                if (on != skin_->texture("", "", "Radio_On") && image(on).valid) {
                    const std::string_view k = c.checked ? "Radio_On" : (hot || focused) ? "Radio_Mouse_Over" : "Radio_Off";
                    if (!quad(dl, layer, skin_->texture(dn, id, k), r)) quad(dl, layer, on, r);
                    draw_text(dl, layer, d, i, c.text, r, TextAlign::Center, false, true, !c.enabled);
                    break;
                }
            }
            if (pushlike) {
                const std::string_view suf = state_suffix(c.type == GuiGadgetType::RadioButton && c.checked);
                auto key = [&](std::string_view part) {
                    std::string k = "Button_";
                    k += part;
                    std::string ks = k + std::string(suf);
                    return image(skin_->texture(dn, id, ks)).valid ? ks : k;
                };
                const std::string l = key("Left"), m = key("Middle"), rr = key("Right");
                const bool any = image(skin_->texture(dn, id, l)).valid || image(skin_->texture(dn, id, m)).valid ||
                                 image(skin_->texture(dn, id, rr)).valid;
                if (any) {
                    three_piece(dl, layer, d, id, l, m, rr, r);
                } else if (style_.fallback_art) {
                    fill(dl, layer, r, down ? style_.fallback_button_down : hot || focused ? style_.fallback_button_hot
                                                                                         : style_.fallback_button);
                }
                if (focused) fill(dl, layer, r, style_.focus);
                draw_text(dl, layer, d, i, c.text, r, TextAlign::Center, false, true, !c.enabled);
            } else {
                std::string k;
                if (c.type == GuiGadgetType::CheckBox) k = c.checked ? "Check_On" : "Check_Off";
                else k = c.checked ? "Radio_On" : (hot ? "Radio_Mouse_Over" : "Radio_Off");
                const GuiImage im = image(skin_->texture(dn, id, k));
                const float art = im.valid ? std::max(im.size.x, im.size.y) : r.height();
                const float sz = c.type == GuiGadgetType::CheckBox ? std::min(kCheckGlyph, art) : std::min(r.height(), art);
                const float cy = 0.5f * (r.min.y + r.max.y);
                const GuiRect ir{{r.min.x, cy - 0.5f * sz}, {r.min.x + sz, cy + 0.5f * sz}};
                if (!quad(dl, layer, skin_->texture(dn, id, k), ir) && style_.fallback_art) {
                    fill(dl, layer, ir, {200, 200, 200, 255});
                    if (c.checked) fill(dl, layer, {{ir.min.x + 2, ir.min.y + 2}, {ir.max.x - 2, ir.max.y - 2}}, {40, 40, 40, 255});
                }
                if (focused) fill(dl, layer, r, style_.focus);
                GuiRect tr = r;
                tr.min.x += sz + 4.0f;
                draw_text(dl, layer, d, i, c.text, tr, TextAlign::Left, false, true, !c.enabled);
            }
            break;
        }
        case GuiGadgetType::Text: {
            // [port] Win32 statics word-wrap; a control less than two lines tall keeps one line (the substituted
            // system fonts are wider than the EmpireAtWar faces the layouts were made for).
            if (c.fit_lines > 0) {
                draw_fit_text(dl, layer, d, i, r);
                break;
            }
            const bool wrap = r.height() >= 2.0f * row_height(d, i);
            draw_text(dl, layer, d, i, c.text, r, c.def->align, wrap, false, !c.enabled);
            break;
        }
        case GuiGadgetType::Frame:
            frame9(dl, layer, d, id, r, false);
            if (!c.text.empty()) {
                GuiRect tr = r;
                tr.min.x += 6.0f;
                draw_text(dl, layer, d, i, c.text, tr, TextAlign::Left, false, false, !c.enabled);
            }
            break;
        case GuiGadgetType::EditBox:
        case GuiGadgetType::IMEEditBox: {
            frame9(dl, layer, d, id, r, true);
            const GuiRect tr = edit_text_rect(d, i);
            const bool has_focus = d.focus_ == static_cast<int32_t>(i);
            const float cy = 0.5f * (r.min.y + r.max.y), h = std::min(r.height() - 4.0f, row_height(d, i));
            if (c.read_only) {  // key-capture field: the whole key name, narrowed to the field (draw_text)
                draw_text(dl, layer, d, i, c.text, tr, TextAlign::Left, false, true, !c.enabled);
                break;
            }
            // Typed text: never narrowed; from scroll_x, as many code points as fit.
            const std::u32string_view all = c.text;
            const size_t s0 = std::min(c.scroll_x, all.size());
            std::u32string_view vis = all.substr(s0);
            while (!vis.empty() && edit_measure(d, i, vis) > tr.width()) vis.remove_suffix(1);
            const size_t lo = std::min(c.caret, c.sel_anchor), hi = std::max(c.caret, c.sel_anchor);
            if (has_focus && lo != hi) {
                const size_t a = std::clamp(lo, s0, s0 + vis.size()), b = std::clamp(hi, s0, s0 + vis.size());
                if (a < b) {
                    const float x0 = tr.min.x + edit_measure(d, i, all.substr(s0, a - s0));
                    const float x1 = tr.min.x + edit_measure(d, i, all.substr(s0, b - s0));
                    fill(dl, layer, {{x0, cy - 0.5f * h}, {x1, cy + 0.5f * h}}, style_.edit_selection);
                }
            }
            draw_text(dl, layer, d, i, vis, tr, TextAlign::Left, false, true, !c.enabled);
            if (has_focus && c.caret >= s0 && c.caret <= s0 + vis.size() &&
                std::fmod(now, 2.0 * style_.edit_caret_blink) < style_.edit_caret_blink) {
                const float x = tr.min.x + edit_measure(d, i, all.substr(s0, c.caret - s0));
                fill(dl, layer, {{x, cy - 0.5f * h}, {x + canvas_.units_per_pixel_x, cy + 0.5f * h}}, {255, 255, 255, 220});
            }
            break;
        }
        case GuiGadgetType::ListBox: {
            frame9(dl, layer, d, id, r, true);
            const float rh = row_height(d, i);
            const int32_t vis = visible_rows(d, i);
            const ScrollBar sb = scroll_bar(d, i);
            const float right = sb.active ? sb.col.min.x : r.max.x;
            for (int32_t k = 0; k < vis; ++k) {
                const int32_t row = c.top + k;
                if (row < 0 || row >= static_cast<int32_t>(c.items.size())) break;
                const float y1 = r.max.y - 2.0f - static_cast<float>(k) * rh;
                const GuiRect rr{{r.min.x + 2.0f, y1 - rh}, {right - 2.0f, y1}};
                if (row == c.selected) fill(dl, layer, rr, style_.selection);
                GuiRect tr = rr;
                tr.min.x += 2.0f;
                {
                    const size_t ri = static_cast<size_t>(row);
                    // UI.md §3.7.1: an item colour recolours the item's text (flat), also on a disabled control.
                    const Rgba8* ic = ri < c.item_colors.size() ? &c.item_colors[ri] : nullptr;
                    const Rgba8* tc = ic ? ic : ri < c.item_text_colors.size() && c.item_text_colors[ri].a > 0 ? &c.item_text_colors[ri] : nullptr;
                    draw_text(dl, layer, d, i, c.items[ri], tr, TextAlign::Left, false, true, !c.enabled && !ic, tc);
                }
            }
            if (sb.active) draw_scroll_bar(dl, layer, d, i, sb);
            break;
        }
        case GuiGadgetType::ComboBox: {
            const GuiRect f = combo_field(d, i);
            const GuiImage btn = image(skin_->texture(dn, id, "Combo_Box_Popdown_Button"));
            const float bw = btn.valid && btn.size.y > 0 ? btn.size.x * f.height() / btn.size.y : f.height();
            three_piece(dl, layer, d, id, "Combo_Box_Left_Cap", "Combo_Box_Text_Box", "", {f.min, {f.max.x - bw, f.max.y}});
            if (!image(skin_->texture(dn, id, "Combo_Box_Text_Box")).valid && style_.fallback_art)
                fill(dl, layer, {f.min, {f.max.x - bw, f.max.y}}, {10, 12, 18, 220});
            const std::string_view bk = c.open ? "Combo_Box_Popdown_Button_Pressed"
                                        : hot  ? "Combo_Box_Popdown_Button_Mouse_Over"
                                               : "Combo_Box_Popdown_Button";
            if (!quad(dl, layer, skin_->texture(dn, id, bk), {{f.max.x - bw, f.min.y}, f.max}) && style_.fallback_art)
                fill(dl, layer, {{f.max.x - bw, f.min.y}, f.max}, style_.fallback_button);
            if (focused) fill(dl, layer, f, style_.focus);
            if (c.selected >= 0 && static_cast<size_t>(c.selected) < c.items.size()) {
                // UI.md §3.7.1: the text is centred in the field left of the pop-down button (left-justified: the field's
                // left edge plus a small inset); when wider than the field less 2 x padding trailing characters are dropped.
                const size_t sel = static_cast<size_t>(c.selected);
                const Rgba8* ic = sel < c.item_colors.size() ? &c.item_colors[sel] : nullptr;
                GuiRect tr{f.min, {f.max.x - bw, f.max.y}};
                const float avail = tr.width() - 2.0f * kFieldInset;
                std::u32string_view txt = c.items[sel];
                if (fonts_) {
                    const HudTextStyle ms = text_style(d, i, false);
                    if (ms.font >= 0)
                        while (!txt.empty() && measure_text_block(*fonts_, canvas_, txt, 0.0f, ms).width > avail) txt.remove_suffix(1);
                }
                if (c.left_justified) tr.min.x += kFieldInset;
                draw_text(dl, layer, d, i, txt, tr, c.left_justified ? TextAlign::Left : TextAlign::Center, false, true, !c.enabled && !ic, ic);
            }
            if (c.open) {  // drop-down list on the popup layer
                const GuiRect lr = combo_list(d, i);
                fill(dl, kPopupLayer, lr, {8, 9, 14, 255});  // [port] opaque under the skin's translucent frame art: the rows it covers do not show through
                frame9(dl, kPopupLayer, d, id, lr, true);
                const float rh = row_height(d, i);
                const ScrollBar sb = scroll_bar(d, i);
                const float right = sb.active ? sb.col.min.x : lr.max.x;
                const int32_t top = std::clamp(c.top, 0, sb.max_top);
                for (int32_t k = 0; k < sb.vis; ++k) {  // rows top .. top + rows - 1 (the list scrolls)
                    const int32_t row = top + k;
                    if (row >= static_cast<int32_t>(c.items.size())) break;
                    const auto ri = static_cast<size_t>(row);
                    const float y1 = lr.max.y - 2.0f - static_cast<float>(k) * rh;
                    const GuiRect rr{{lr.min.x + 2.0f, y1 - rh}, {right - 2.0f, y1}};
                    if (row == c.selected || rr.contains(cursor_)) fill(dl, kPopupLayer, rr, style_.selection);
                    GuiRect tr = rr;
                    tr.min.x += 2.0f;
                    draw_text(dl, kPopupLayer, d, i, c.items[ri], tr, TextAlign::Left, false, true, false,
                              ri < c.item_colors.size() ? &c.item_colors[ri] : nullptr);
                }
                if (sb.active) draw_scroll_bar(dl, kPopupLayer, d, i, sb);
            }
            break;
        }
        case GuiGadgetType::TrackbarSlider: {
            const GuiImage mid = image(skin_->texture(dn, id, "Dial_Middle"));
            const float th = mid.valid ? std::min(r.height(), mid.size.y) : std::min(r.height(), 6.0f);
            const float cy = 0.5f * (r.min.y + r.max.y);
            const GuiRect track{{r.min.x, cy - 0.5f * th}, {r.max.x, cy + 0.5f * th}};
            if (mid.valid) three_piece(dl, layer, d, id, "Dial_Left", "Dial_Middle", "Dial_Right", track);
            else if (style_.fallback_art) fill(dl, layer, track, {60, 66, 80, 255});
            const float t = c.range_max > c.range_min
                                ? static_cast<float>(c.value - c.range_min) / static_cast<float>(c.range_max - c.range_min)
                                : 0.0f;
            const GuiImage tab = image(skin_->texture(dn, id, "Dial_Tab"));
            const Vec2f ts = tab.valid ? Vec2f{std::min(tab.size.x, r.height()), std::min(tab.size.y, r.height())}
                                       : Vec2f{6.0f, r.height()};
            const float x = r.min.x + 0.5f * ts.x + (r.width() - ts.x) * t;
            const GuiRect tr{{x - 0.5f * ts.x, cy - 0.5f * ts.y}, {x + 0.5f * ts.x, cy + 0.5f * ts.y}};
            if (!quad(dl, layer, skin_->texture(dn, id, "Dial_Tab"), tr) && style_.fallback_art)
                fill(dl, layer, tr, hot ? style_.fallback_button_hot : Rgba8{200, 190, 120, 255});
            if (focused) fill(dl, layer, r, style_.focus);
            break;
        }
        case GuiGadgetType::ProgressBar: {
            const float t = c.range_max > c.range_min
                                ? std::clamp(static_cast<float>(c.value - c.range_min) / static_cast<float>(c.range_max - c.range_min),
                                             0.0f, 1.0f)
                                : 0.0f;
            if (image(skin_->texture(dn, id, "Progress_Bar_Middle_Off")).valid) {
                three_piece(dl, layer, d, id, "Progress_Bar_Left", "Progress_Bar_Middle_Off", "Progress_Bar_Right", r);
                three_piece(dl, layer, d, id, "", "Progress_Bar_Middle_On", "", r, t);
            } else if (style_.fallback_art) {
                fill(dl, layer, r, {30, 34, 42, 230});
                fill(dl, layer, {{r.min.x + 1, r.min.y + 1}, {r.min.x + 1 + (r.width() - 2) * t, r.max.y - 1}}, {80, 170, 255, 255});
            }
            break;
        }
        case GuiGadgetType::CustomImage:
            if (!c.image.empty()) quad(dl, layer, c.image, r, c.tint);
            {  // UI.md 9.6.1: each label centred on its point, in the control's font, inside the rect inset by 5 %
                const float ax = r.min.x + 0.05f * r.width(), ay = r.max.y - 0.05f * r.height();
                const float aw = 0.9f * r.width(), ah = 0.9f * r.height();
                for (const GuiMarker& m : c.markers) {
                    const float mx = ax + m.pos.x * aw, my = ay - m.pos.y * ah;  // pos.y runs down, the canvas runs up
                    draw_text(dl, layer, d, i, m.label, {{mx, my}, {mx, my}}, TextAlign::Center, false, true, false, &m.color);
                }
            }
            break;
        default: break;
    }
}

void GuiManager::build(HudDrawList& dl, double now) {
    // Backdrop hotspots follow the stretched overlay (one quad over the whole canvas, 1024 x 768 art): their 4:3 rectangle
    // (the column dialog's right edge on the base area's right edge, x relative to the canvas centre, y down from the
    // dialog's top edge = the canvas top) is stretched by canvas width / 1024 and canvas height / 768. A column dialog
    // keeps its top on the canvas top at every aspect, so its y stretches too (taller-than-4:3 windows); other dialogs
    // are centred and only x follows.
    const float sx = canvas_.width / kHudBaseWidth, sy = canvas_.height / kHudBaseHeight;
    for (auto& dp : stack_) {
        GuiDialog& d = *dp;
        const auto w = static_cast<float>(d.def().w);
        const bool column = static_cast<float>(d.def().h) > canvas_.height;
        const float base_x = column ? 0.5f * kHudBaseWidth - w : d.origin().x;
        for (size_t i = 0; i < d.size(); ++i) {
            GuiControl& c = d[i];
            if (!c.def) continue;
            bool hot = false;
            for (const std::string& id : backdrop_hotspots_) hot = hot || iequals(c.def->id, id);
            if (!hot) continue;
            const float base_left = base_x + static_cast<float>(c.def->x);
            c.scale.x = sx;
            c.offset.x = base_left * sx - (d.origin().x + static_cast<float>(c.def->x));
            c.scale.y = column ? sy : 1.0f;
            c.offset.y = column ? static_cast<float>(c.def->y) * (1.0f - sy) : 0.0f;
        }
    }
    if (!skin_ || !fonts_) return;
    if (!backdrop_.empty()) {
        // GALAXY_MAP_PRESENTATION §5: the overlay is one full-window quad, stretched to any aspect.
        const GuiRect all{{-0.5f * canvas_.width, -0.5f * canvas_.height}, {0.5f * canvas_.width, 0.5f * canvas_.height}};
        fill(dl, 0, all, {0, 0, 0, 255});
        quad(dl, 0, backdrop_, all);
        if (!version_text_.empty() && fonts_) {
            HudTextStyle st;
            st.font = fonts_->font("Arial", 8.0f, canvas_.screen_height);
            if (st.font >= 0) {
                st.color = Rgba8{40, 160, 255, 128};
                const float lh = static_cast<float>(fonts_->metrics(st.font).line_height) * canvas_.units_per_pixel_y;
                const Vec2f at{all.min.x + 2.0f * canvas_.units_per_pixel_x, all.min.y + 2.0f * canvas_.units_per_pixel_y + 0.5f * lh};
                add_text(dl, 0, HudList::Text, *fonts_, canvas_, version_text_, at, st);
            }
        }
    }
    for (size_t k = 0; k < stack_.size(); ++k) {
        const GuiDialog& d = *stack_[k];
        const auto layer = static_cast<uint32_t>(std::min<size_t>(k, kHudLayers - 2));
        if (d.options().opaque_backdrop)
            fill(dl, layer, {{-0.5f * canvas_.width, -0.5f * canvas_.height}, {0.5f * canvas_.width, 0.5f * canvas_.height}}, {0, 0, 0, 255});
        frame16(dl, layer, d);
        for (size_t i = 0; i < d.size(); ++i) draw_control(dl, layer, d, i, now);
    }
    // Tooltip (GUIDialogs.xml <Tooltips>) after Tooltip_Delay of hover, above-right of the cursor [port].
    if (hover_dialog_ && hover_control_ >= 0 && now - hover_since_ >= style_.tooltip_delay) {
        const GuiControl& c = (*hover_dialog_)[static_cast<size_t>(hover_control_)];
        const std::string_view key = skin_->tooltip(c.id());
        if (!key.empty()) {
            const std::u32string t = caption_text(key);
            HudTextStyle s;
            s.font = fonts_->font(skin_->global_font().face, skin_->global_font().size, canvas_.screen_height);
            s.color = {255, 255, 255, 255};
            const TextBlock b = measure_text_block(*fonts_, canvas_, t, 200.0f, s);
            const GuiRect box{{cursor_.x + 12.0f, cursor_.y + 8.0f}, {cursor_.x + 20.0f + b.width, cursor_.y + 12.0f + b.height}};
            fill(dl, kPopupLayer, box, {0, 0, 0, 200});
            add_text_block(dl, kPopupLayer, HudList::Text, *fonts_, canvas_, t, {box.min.x + 4.0f, box.max.y - 2.0f}, 200.0f, s);
        }
    }
}

}  // namespace alamo::ui
