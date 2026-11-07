#include "alamo/ui/hud_components.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::ui {

namespace {

std::vector<std::string> names(std::string_view text) {
    std::vector<std::string> out;
    for (std::string_view t : data::split_list(text)) out.emplace_back(t);
    return out;
}

Vec2f vec2(std::string_view text) {
    float v[2] = {0, 0};
    const size_t n = data::parse_floats(text, v, 2);
    if (n == 1) v[1] = 0.0f;
    return {v[0], v[1]};
}

Rgba8 rgba(std::string_view text) {
    float v[4] = {255, 255, 255, 255};
    data::parse_floats(text, v, 4);
    auto c = [](float f) {
        f = std::clamp(f, 0.0f, 255.0f);
        return static_cast<uint8_t>(std::lround(f));
    };
    return {c(v[0]), c(v[1]), c(v[2]), c(v[3])};
}

ComponentType parse_type(std::string_view t) {
    t = data::trim(t);
    if (iequals(t, "Shell")) return ComponentType::Shell;
    if (iequals(t, "Button")) return ComponentType::Button;
    if (iequals(t, "TextButton")) return ComponentType::TextButton;
    if (iequals(t, "Bar")) return ComponentType::Bar;
    if (iequals(t, "Icon")) return ComponentType::Icon;
    return ComponentType::Unknown;
}

void apply_field(CommandBarComponent& c, const std::string& tag, std::string_view text) {
    auto is = [&](const char* n) { return tag == n; };
    auto b = [&](bool fallback) { return data::parse_bool_or(text, fallback); };
    if (is("TYPE")) c.type = parse_type(text);
    else if (is("GROUP")) c.group = std::string(data::trim(text));
    else if (is("MODEL_NAME")) c.model_name = std::string(data::trim(text));
    else if (is("MEGA_TEXTURE_NAME")) c.mega_texture = std::string(data::trim(text));
    else if (is("MODEL_OFFSET_X")) c.model_offset_x = b(false);
    else if (is("MODEL_OFFSET_Y")) c.model_offset_y = b(false);
    else if (is("MANUAL_OFFSET")) c.manual_offset = b(false);
    else if (is("HIDDEN")) c.hidden = b(false);
    else if (is("DISABLED")) c.disabled = b(false);
    else if (is("NO_SHELL")) c.no_shell = b(false);
    else if (is("BLANK_TEXTURE_NAME")) c.blank_texture = names(text);
    else if (is("ICON_TEXTURE_NAME")) c.icon_texture = names(text);
    else if (is("ICON_ALTERNATE_TEXTURE_NAME")) c.icon_alternate_texture = names(text);
    else if (is("MOUSE_OVER_TEXTURE_NAME")) c.mouse_over_texture = names(text);
    else if (is("SELECTED_TEXTURE_NAME")) c.selected_texture = names(text);
    else if (is("DISABLED_TEXTURE_NAME")) c.disabled_texture = names(text);
    else if (is("FLASH_TEXTURE_NAME")) c.flash_texture = names(text);
    else if (is("BUILD_TEXTURE_NAME")) c.build_texture = names(text);
    else if (is("BAR_TEXTURE_NAME")) c.bar_texture = names(text);
    else if (is("BAR_OVERLAY_NAME")) c.bar_overlay = names(text);
    else if (is("LOWER_EFFECT_TEXTURE_NAME")) c.lower_effect_texture = names(text);
    else if (is("UPPER_EFFECT_TEXTURE_NAME")) c.upper_effect_texture = names(text);
    else if (is("OVERLAY_TEXTURE_NAME")) c.overlay_texture = names(text);
    else if (is("OVERLAY2_TEXTURE_NAME")) c.overlay2_texture = names(text);
    else if (is("BASE_LAYER")) c.base_layer = std::clamp(data::parse_int_or(text, 0), 0, static_cast<int32_t>(kHudLayers - 1));
    else if (is("SIZE")) {
        c.size = vec2(text);
        c.has_size = true;
    } else if (is("SCALE")) c.scale = data::parse_float_or(text, 1.0f);
    else if (is("OFFSET")) c.offset = vec2(text);
    else if (is("DEFAULT_OFFSET")) c.default_offset = vec2(text);
    else if (is("DEFAULT_OFFSET_WIDESCREEN")) {
        c.default_offset_widescreen = vec2(text);
        c.has_default_offset_widescreen = true;
    } else if (is("ICON_OFFSET")) c.icon_offset = vec2(text);
    else if (is("TEXT_OFFSET")) c.text_offset = vec2(text);
    else if (is("TEXT_OFFSET2")) c.text_offset2 = vec2(text);
    else if (is("BUILD_DIAL_OFFSET")) c.build_dial_offset = vec2(text);
    else if (is("BUILD_DIAL2_OFFSET")) c.build_dial2_offset = vec2(text);
    else if (is("OVERLAY_OFFSET")) c.overlay_offset = vec2(text);
    else if (is("OVERLAY2_OFFSET")) c.overlay2_offset = vec2(text);
    else if (is("PIXEL_ALIGN")) c.pixel_align = b(true);
    else if (is("FONT_NAME")) c.font_name = std::string(data::trim(text));
    else if (is("ALTERNATE_FONT_NAME")) c.alternate_font_name = std::string(data::trim(text));
    else if (is("FONT_POINT_SIZE")) c.font_point_size = data::parse_float_or(text, 0.0f);
    else if (is("COLOR")) {
        c.color = rgba(text);
        c.has_color = true;
    } else if (is("TEXT_COLOR")) {
        c.text_color = rgba(text);
        c.has_text_color = true;
    } else if (is("TEXT_COLOR2")) {
        c.text_color2 = rgba(text);
        c.has_text_color2 = true;
    } else if (is("TEXT_OUTLINE")) c.text_outline = b(false);
    else if (is("TEXT_EMBOSS")) c.text_emboss = b(false);
    else if (is("LEFT_JUSTIFIED")) c.left_justified = b(false);
    else if (is("RIGHT_JUSTIFIED")) c.right_justified = b(false);
    else if (is("MAX_TEXT_WIDTH")) c.max_text_width = data::parse_float_or(text, 0.0f);
    else if (is("TOOLTIP_TEXT")) c.tooltip_text = names(text);
    else if (is("MAX_BAR_LEVEL")) c.max_bar_level = std::clamp(data::parse_int_or(text, 10), 0, 64);
    else if (is("SMOOTH_BAR")) c.smooth_bar = b(false);
    else if (is("OUTLINED_BAR")) c.outlined_bar = b(false);
    else if (is("LOWER_EFFECT_ADDITIVE")) c.lower_effect_additive = b(false);
    else if (is("MOUSE_OVER_SFX")) c.mouse_over_sfx = std::string(data::trim(text));
    else if (is("CLICK_SFX")) c.click_sfx = std::string(data::trim(text));
    else if (is("RIGHT_CLICK_SFX")) c.right_click_sfx = std::string(data::trim(text));
    else if (is("TOGGLE")) c.toggle = b(false);
    else if (is("TAB")) c.tab = b(false);
    else if (is("SELECTED_ALPHA")) c.selected_alpha = b(false);
    else if (is("DISABLE_DARKEN") || is("DISABLED_DARKEN")) c.disabled_darken = b(false);
    else if (is("CLICK_SHIFT")) c.click_shift = b(false);
    else if (is("NO_HIDDEN_COLLISION")) c.no_hidden_collision = b(false);
    else if (is("CAN_ANIMATE")) c.can_animate = b(false);
    else if (is("LOOP_ANIM")) c.loop_anim = b(false);
    else if (is("ANIM_FPS")) c.anim_fps = data::parse_float_or(text, 0.0f);
    else if (is("BLINK_DURATION")) c.blink_duration = data::parse_float_or(text, 0.0f);
    else if (is("BLINK_RATE")) c.blink_rate = data::parse_float_or(text, 0.0f);
    else if (is("BLINK_FADE")) c.blink_fade = b(false);
    else if (is("MOUSE_OVER_OFFSET")) c.mouse_over_offset = vec2(text);
    else if (is("DISABLED_OFFSET")) c.disabled_offset = vec2(text);
    else if (is("UPPER_EFFECT_OFFSET")) c.upper_effect_offset = vec2(text);
    else if (is("LOWER_EFFECT_OFFSET")) c.lower_effect_offset = vec2(text);
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::string_view CommandBarComponent::raw_value(std::string_view tag) const {
    const auto it = raw.find(ascii_upper(tag));
    return it == raw.end() ? std::string_view{} : std::string_view(it->second);
}

// ---- ComponentTable ------------------------------------------------------------------------------
size_t ComponentTable::parse(const data::XmlNode& root) {
    size_t n = 0;
    for (data::XmlNode e : root.children()) {
        std::string_view name = e.attribute("Name");
        if (name.empty() && !iequals(e.name(), "CommandBarComponent")) name = e.name();
        name = data::trim(name);
        if (name.empty()) continue;
        CommandBarComponent c;
        c.name = std::string(name);
        for (data::XmlNode f : e.children()) {
            const std::string tag = ascii_upper(f.name());
            const std::string_view text = f.text_trimmed();
            c.raw[tag] = std::string(text);
            apply_field(c, tag, text);
        }
        const std::string key = ascii_upper(c.name);
        const auto it = index_.find(key);
        if (it != index_.end()) {
            items_[it->second] = std::move(c);
        } else {
            index_.emplace(key, static_cast<uint32_t>(items_.size()));
            items_.push_back(std::move(c));
        }
        ++n;
    }
    return n;
}

bool ComponentTable::load(const FileProvider& files, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\CommandBarComponentFiles.xml", bytes)) {
        if (error) *error = "CommandBarComponentFiles.xml not found";
        return false;
    }
    const data::XmlDocument list = data::XmlDocument::parse(bytes, "CommandBarComponentFiles.xml");
    if (!list.ok()) {
        if (error) *error = "CommandBarComponentFiles.xml: no root";
        return false;
    }
    for (data::XmlNode f : list.root().children("File")) {
        const std::string name(f.text_trimmed());
        if (name.empty()) continue;
        std::vector<uint8_t> xb;
        const std::string path = "Data\\XML\\" + name;
        if (!files.read(path, xb)) {
            if (error) *error += "missing " + path + "; ";
            continue;
        }
        const data::XmlDocument doc = data::XmlDocument::parse(xb, name);
        if (!doc.ok()) {
            if (error) *error += "unreadable " + path + "; ";
            continue;
        }
        parse(doc.root());
    }
    return true;
}

int32_t ComponentTable::find(std::string_view name) const {
    const auto it = index_.find(ascii_upper(data::trim(name)));
    return it == index_.end() ? -1 : static_cast<int32_t>(it->second);
}

uint32_t layer_of(const CommandBarComponent& c, const ComponentPlacement& p) {
    int32_t l = c.base_layer;
    if (p.from_bone) l = std::max(l, kShellLayerMin);
    return static_cast<uint32_t>(std::clamp(l, 0, static_cast<int32_t>(kHudLayers) - 1));
}

Vec2f component_art_size(const CommandBarComponent& c, const ComponentPlacement& p, const TextureAtlas& atlas, Vec2f fallback) {
    if (p.size.x > 0 && p.size.y > 0) return p.size;
    if (c.has_size) return c.size;
    for (const auto* list : {&c.blank_texture, &c.icon_texture, &c.icon_alternate_texture})
        if (!list->empty())
            if (const AtlasEntry* e = atlas.find(list->front()))
                return {static_cast<float>(e->width) * c.scale, static_cast<float>(e->height) * c.scale};
    return fallback;
}

int32_t find_fmt(const ComponentTable& t, const char* fmt, int a, int b) {
    char n[48];
    if (b < 0) std::snprintf(n, sizeof n, fmt, a);
    else std::snprintf(n, sizeof n, fmt, a, b);
    return t.find(n);
}

// ---- Atlas --------------------------------------------------------------------------------------
std::string atlas_key(std::string_view n) {
    n = data::trim(n);
    const size_t slash = n.find_last_of("\\/");
    if (slash != std::string_view::npos) n = n.substr(slash + 1);
    const size_t dot = n.find('.');
    if (dot != std::string_view::npos) n = n.substr(0, dot);
    return ascii_upper(n);
}

bool TextureAtlas::parse(std::span<const uint8_t> bytes, std::string* error) {
    entries_.clear();
    index_.clear();
    if (bytes.size() < 4) {
        if (error) *error = "MTD: truncated header";
        return false;
    }
    const uint32_t count = rd32(bytes.data());
    constexpr size_t kRecord = 81;
    if (count > (bytes.size() - 4) / kRecord) {
        if (error) *error = "MTD: truncated records";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = bytes.data() + 4 + static_cast<size_t>(i) * kRecord;
        AtlasEntry e;
        size_t len = 0;
        while (len < 64 && p[len] != 0) ++len;
        e.name.assign(reinterpret_cast<const char*>(p), len);
        e.x = rd32(p + 64);
        e.y = rd32(p + 68);
        e.width = rd32(p + 72);
        e.height = rd32(p + 76);
        e.alpha = p[80] != 0;
        add(std::move(e));
    }
    return true;
}

void TextureAtlas::add(AtlasEntry e) {
    const std::string key = atlas_key(e.name);
    const auto it = index_.find(key);
    if (it != index_.end()) {
        entries_[it->second] = std::move(e);  // later entries win [port]
        return;
    }
    index_.emplace(key, static_cast<uint32_t>(entries_.size()));
    entries_.push_back(std::move(e));
}

const AtlasEntry* TextureAtlas::find(std::string_view texture_name) const {
    const auto it = index_.find(atlas_key(texture_name));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

// ---- Text database ------------------------------------------------------------------------------
bool TextDatabase::parse(std::span<const uint8_t> bytes, std::string* error) {
    crcs_.clear();
    keys_.clear();
    values_.clear();
    added_.clear();
    loaded_ = false;
    auto fail = [&](const char* m) {
        if (error) *error = m;
        crcs_.clear();
        keys_.clear();
        values_.clear();
        return false;
    };
    if (bytes.size() < 4) return fail("DAT: truncated header");
    const uint32_t count = rd32(bytes.data());
    if (count > (bytes.size() - 4) / 12) return fail("DAT: truncated records");
    size_t value_chars = 0, key_chars = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = bytes.data() + 4 + static_cast<size_t>(i) * 12;
        value_chars += rd32(r + 4);
        key_chars += rd32(r + 8);
    }
    const size_t head = 4 + static_cast<size_t>(count) * 12;
    if (value_chars > (bytes.size() - head) / 2 || key_chars > bytes.size() - head - value_chars * 2)
        return fail("DAT: truncated strings");
    size_t vo = head, ko = head + value_chars * 2;
    crcs_.reserve(count);
    keys_.reserve(count);
    values_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = bytes.data() + 4 + static_cast<size_t>(i) * 12;
        const uint32_t vc = rd32(r + 4), kc = rd32(r + 8);
        std::u16string v(vc, u'\0');
        for (uint32_t k = 0; k < vc; ++k)
            v[k] = static_cast<char16_t>(bytes[vo + 2 * k] | (bytes[vo + 2 * k + 1] << 8));
        vo += static_cast<size_t>(vc) * 2;
        std::string key(reinterpret_cast<const char*>(bytes.data() + ko), kc);
        ko += kc;
        crcs_.push_back(rd32(r));
        keys_.push_back(std::move(key));
        values_.push_back(std::move(v));
    }
    sort();
    added_.clear();
    loaded_ = true;  // byte +0, the "loaded" flag (GALACTIC_DIALOGS §2.7)
    return true;
}

void TextDatabase::sort() {
    std::vector<uint32_t> order(keys_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return crcs_[a] != crcs_[b] ? crcs_[a] < crcs_[b] : keys_[a] < keys_[b];
    });
    std::vector<uint32_t> c(order.size());
    std::vector<std::string> k(order.size());
    std::vector<std::u16string> v(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        c[i] = crcs_[order[i]];
        k[i] = std::move(keys_[order[i]]);
        v[i] = std::move(values_[order[i]]);
    }
    crcs_ = std::move(c);
    keys_ = std::move(k);
    values_ = std::move(v);
}

void TextDatabase::add(std::string key, std::u16string value) {
    crcs_.push_back(crc32(key.data(), key.size()));
    keys_.push_back(std::move(key));
    values_.push_back(std::move(value));
    sort();
    loaded_ = true;
}

const std::u16string* TextDatabase::find(std::string_view key) const {
    const uint32_t c = crc32(key.data(), key.size());
    auto it = std::lower_bound(crcs_.begin(), crcs_.end(), c);
    for (; it != crcs_.end() && *it == c; ++it) {
        const auto i = static_cast<size_t>(it - crcs_.begin());
        if (keys_[i] == key) return &values_[i];
    }
    return nullptr;  // case-sensitive: no case-folded second search (GALACTIC_DIALOGS §2.7)
}

std::u16string TextDatabase::text(std::string_view key) const { return text(key, true); }

std::u16string TextDatabase::text(std::string_view key, bool add) const {
    if (key.data() == nullptr || !loaded_) return std::u16string(kMissing);
    const uint32_t c = crc32(key.data(), key.size());
    // The runtime additions first, then the file's table.
    if (const auto it = added_.find({c, std::string(key)}); it != added_.end()) return it->second;
    if (const std::u16string* v = find(key)) return *v;
    if (!add) return std::u16string(kMissing);
    // Missing: the key itself (UTF-8 -> UTF-16, MultiByteToWideChar code page 65001), remembered under its CRC.
    std::u16string out;
    for (const char32_t cp : utf8_to_utf32(key)) {
        if (cp >= 0x10000) {
            out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    added_.emplace(std::pair<uint32_t, std::string>{c, std::string(key)}, out);
    return out;
}

void TextDatabase::pseudo_localize_all() {
    for (std::u16string& v : values_) v = pseudo_localize(v);
    pseudo_ = true;
}

std::u32string TextDatabase::port_text(std::u32string_view english) const {
    if (!pseudo_) return std::u32string(english);
    std::u16string s;  // port strings are plain ASCII
    for (char32_t ch : english) s.push_back(static_cast<char16_t>(ch));
    return utf16_to_utf32(pseudo_localize(s));
}

std::u16string pseudo_localize(std::u16string_view s) {
    const auto is_digit = [](char16_t c) { return c >= u'0' && c <= u'9'; };
    const auto is_letter = [](char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); };
    // Length of the token at `i` that must stay as written, 0 = none.
    const auto kept = [&](size_t i) -> size_t {
        const size_t n = s.size();
        if (s[i] == u'\\') return i + 1 < n ? 2 : 1;
        if (s[i] == u'#' && i + 2 < n && s[i + 1] == u'#' && is_digit(s[i + 2])) return 3;
        if (s[i] != u'%') return 0;
        if (i + 1 < n && s[i + 1] == u'%') return 2;
        size_t j = i + 1;  // no space flag: "50% due" is no spec
        while (j < n && (s[j] == u'+' || s[j] == u'-' || s[j] == u'#' || s[j] == u'0')) ++j;
        while (j < n && is_digit(s[j])) ++j;
        if (j < n && s[j] == u'.') {
            ++j;
            while (j < n && is_digit(s[j])) ++j;
        }
        while (j < n && (s[j] == u'l' || s[j] == u'h')) ++j;
        if (j < n && std::u16string_view(u"sdiufeExXgGc").find(s[j]) != std::u16string_view::npos) return j + 1 - i;
        return 0;
    };
    size_t letters = 0;
    for (size_t i = 0; i < s.size();) {
        if (const size_t k = kept(i)) {
            i += k;
        } else {
            letters += is_letter(s[i]) ? 1u : 0u;
            ++i;
        }
    }
    if (letters == 0) return std::u16string(s);
    const auto accent = [](char16_t c) -> char16_t {
        switch (c) {
            case u'a': return u'á';
            case u'c': return u'ç';
            case u'e': return u'é';
            case u'i': return u'í';
            case u'n': return u'ñ';
            case u'o': return u'ó';
            case u'u': return u'ú';
            case u'y': return u'ý';
            case u'A': return u'Á';
            case u'C': return u'Ç';
            case u'E': return u'É';
            case u'I': return u'Í';
            case u'N': return u'Ñ';
            case u'O': return u'Ó';
            case u'U': return u'Ú';
            case u'Y': return u'Ý';
            default: return c;
        }
    };
    const size_t added = std::max<size_t>(2, (7 * s.size() + 10) / 20);  // 35 %, rounded; the brackets count
    const size_t doubled = added - 2;
    std::u16string out;
    out.reserve(s.size() + added);
    out.push_back(u'[');
    size_t letter = 0;
    for (size_t i = 0; i < s.size();) {
        if (const size_t k = kept(i)) {
            out.append(s.substr(i, k));
            i += k;
            continue;
        }
        const char16_t c = s[i++];
        if (!is_letter(c)) {
            out.push_back(c);
            continue;
        }
        // Evenly over the letters: the repeat count steps with the letter index.
        out.append(1 + ((letter + 1) * doubled) / letters - (letter * doubled) / letters, accent(c));
        ++letter;
    }
    out.push_back(u']');
    return out;
}

std::string master_text_path(std::string_view language) {
    return "Data\\Text\\MasterTextFile_" + ascii_upper(language) + ".DAT";
}

bool load_master_text(const FileProvider& files, std::string_view language, TextDatabase& out, std::string* log) {
    std::vector<uint8_t> bytes;
    std::string lang = ascii_upper(language.empty() ? std::string_view("ENGLISH") : language);
    if (!files.read(master_text_path(lang), bytes)) {
        if (log) *log += "No " + lang + " MasterTextFile exists. Falling back to ENGLISH\n";
        lang = "ENGLISH";
        if (!files.read(master_text_path(lang), bytes)) return false;
    }
    std::string err;
    if (!out.parse(bytes, &err)) {
        if (log) *log += err + "\n";
        return false;
    }
    out.language = lang;
    return true;
}

std::u32string utf16_to_utf32(std::u16string_view s) {
    std::u32string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            o.push_back(0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00));
            ++i;
        } else {
            o.push_back(c);
        }
    }
    return o;
}

std::u32string utf8_to_utf32(std::string_view s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            o.push_back(0xFFFD);
            ++i;
            continue;
        }
        char32_t c = n == 1 ? b : n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
        for (size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        o.push_back(c);
        i += n;
    }
    return o;
}

// ---- Layout -------------------------------------------------------------------------------------
void HudLayout::set_shell_bones(uint32_t shell_component, std::vector<ShellBone> bones) {
    shells_[shell_component].bones = std::move(bones);
}

Vec2f shell_anchor_offset(const CommandBarComponent& shell, std::span<const ShellBone> bones, const HudCanvas& canvas) {
    Vec2f off{};
    if (bones.empty()) return off;
    // P4 completion 2 [port]: Dialog_Scene shells (market_frame, bm_frame, corruption_shell, hack_shell, map_overlay_shell)
    // are full-screen dialog scenes whose meshes span the 1024-unit base area around the model origin (x -512..512 in
    // i_black_market.alo / i_hack.alo): the origin is the canvas centre, Model_Offset_X / _Y notwithstanding.
    if (const std::string_view ds = shell.raw_value("DIALOG_SCENE");
        data::parse_flag_char(ds))
        return off;
    // GALACTIC_DIALOGS §1.2 / §4: horizontally centred shells stuck to the top edge (Model_Offset_X False, _Y True).
    //   Tutorial_Scene banners (pending_battle_shell, story_pending_battle_shell, pause_shell, autoresolve_shell): model
    //   y 92 on the top edge [port: calibrated on 28.jpg, buttons 46 / title 8 units below the top];
    //   i_main_organize (no Manual_Offset): model origin 384 units below the top edge = the canvas centre (768-unit
    //   canvas) [port: calibrated on 26.jpg: Text_Fleet at y 90 is 295 units below the top].
    if (!shell.model_offset_x && shell.model_offset_y) {
        const std::string_view ts = shell.raw_value("TUTORIAL_SCENE");
        if (data::parse_flag_char(ts))
            return {0.0f, 0.5f * canvas.height - 92.0f};
        if (!shell.manual_offset) return {0.0f, 0.5f * canvas.height - 384.0f};
    }
    double sx = 0, sy = 0;
    for (const ShellBone& b : bones) {
        sx += b.x;
        sy += b.y;
    }
    const auto n = static_cast<double>(bones.size());
    const auto mx = static_cast<float>(sx / n), my = static_cast<float>(sy / n);
    auto edge = [](bool flag, float mean, float half) {
        if (!flag) return 0.0f;
        if (mean > 1.0f) return -half;  // bones extend right / up from the origin: origin on the left / bottom edge
        if (mean < -1.0f) return half;
        return 0.0f;
    };
    off.x = edge(shell.model_offset_x, mx, 0.5f * canvas.width);
    off.y = edge(shell.model_offset_y, my, 0.5f * canvas.height);
    return off;
}

void HudLayout::set_shell_active(uint32_t shell_component, bool active) {
    const auto it = shells_.find(shell_component);
    if (it != shells_.end()) it->second.active = active;
}

bool HudLayout::shell_active(uint32_t shell_component) const {
    const auto it = shells_.find(shell_component);
    return it != shells_.end() && it->second.active;
}

std::vector<uint32_t> HudLayout::shells() const {
    std::vector<uint32_t> out;
    for (const auto& [idx, s] : shells_)
        if (!s.bones.empty()) out.push_back(idx);
    return out;
}

void HudLayout::update(const HudCanvas& canvas, const ComponentTable& table) {
    placements_.assign(table.size(), ComponentPlacement{});
    for (auto& [idx, shell] : shells_) {
        shell.offset = {};
        if (idx >= table.size() || shell.bones.empty()) continue;
        shell.offset = shell_anchor_offset(table[idx], shell.bones, canvas);
    }
    // Bones: the first active shell (ascending component index) with a bone of the component's name wins.
    std::map<std::string, std::pair<uint32_t, const ShellBone*>> bone_index;  // lookup only
    for (const auto& [idx, shell] : shells_) {
        if (idx >= table.size() || !shell.active) continue;
        for (const ShellBone& b : shell.bones) bone_index.emplace(ascii_upper(b.name), std::make_pair(idx, &b));
    }
    for (uint32_t i = 0; i < table.size(); ++i) {
        ComponentPlacement& p = placements_[i];
        const CommandBarComponent& c = table[i];
        if (c.type == ComponentType::Shell) continue;
        const auto it = c.no_shell ? bone_index.end() : bone_index.find(ascii_upper(c.name));
        if (it != bone_index.end()) {
            const Vec2f off = shells_[it->second.first].offset;
            const ShellBone& b = *it->second.second;
            p.placed = p.from_bone = true;
            p.shell = static_cast<int32_t>(it->second.first);
            p.center = {b.x + off.x, b.y + off.y};
            p.size = {2.0f * b.half_size.x, 2.0f * b.half_size.y};
            p.rect_center = {p.center.x + b.rect_center.x, p.center.y + b.rect_center.y};
            p.z = b.z;
            continue;
        }
        const auto fb = fallback_.find(i);
        if (fb != fallback_.end()) {
            p.placed = true;
            p.center = p.rect_center = fb->second;
        }
    }
}

const ComponentPlacement& HudLayout::placement(uint32_t component) const {
    static const ComponentPlacement kNone{};
    return component < placements_.size() ? placements_[component] : kNone;
}

Vec2f HudLayout::shell_offset(uint32_t shell_component) const {
    const auto it = shells_.find(shell_component);
    return it == shells_.end() ? Vec2f{} : it->second.offset;
}

std::span<const ShellBone> HudLayout::shell_bones(uint32_t shell_component) const {
    const auto it = shells_.find(shell_component);
    return it == shells_.end() ? std::span<const ShellBone>{} : std::span<const ShellBone>(it->second.bones);
}

// [port] skeleton positions (canvas units, +Y up) used until the shell .alo bones are available.
void apply_tactical_fallback_layout(const ComponentTable& table, const HudCanvas& canvas, HudLayout& layout) {
    const float hw = 0.5f * canvas.width, hh = 0.5f * canvas.height;
    auto put = [&](std::string_view name, float x, float y) {
        const int32_t i = table.find(name);
        if (i >= 0) layout.set_fallback(static_cast<uint32_t>(i), {x, y});
    };
    put("radar", -hw + 92.0f, -hh + 94.0f);
    // Selection panel: 48 slots, 12 columns x 4 rows of 44 units, bottom centre (row 0 at the bottom).
    for (int s = 0; s < 48; ++s) {
        const int col = s % 12, row = s / 12;
        const float x = (static_cast<float>(col) - 5.5f) * 44.0f + 40.0f;
        const float y = -hh + 30.0f + static_cast<float>(3 - row) * 44.0f;
        char n[32];
        std::snprintf(n, sizeof n, "s_select_%02d", s);
        put(n, x, y);
        std::snprintf(n, sizeof n, "s_health_%02d", s);
        put(n, x, y - 19.0f);
        std::snprintf(n, sizeof n, "s_shield_%02d", s);
        put(n, x, y - 23.0f);
    }
    // Ability buttons: two per selection column above the grid; build queue above them.
    for (int s = 0; s < 24; ++s) {
        char n[32];
        std::snprintf(n, sizeof n, "special_button_%02d", s);
        put(n, (static_cast<float>(s / 2) - 5.5f) * 44.0f + 40.0f + (s % 2 ? 11.0f : -11.0f), -hh + 198.0f);
    }
    for (int q = 0; q < 10; ++q) {
        char n[32];
        std::snprintf(n, sizeof n, "tqueue%02d", q);
        put(n, (static_cast<float>(q) - 4.5f) * 56.0f + 40.0f, -hh + 240.0f);
    }
    for (int c = 0; c < 6; ++c) {
        char n[32];
        std::snprintf(n, sizeof n, "c_button%02d", c);
        put(n, -hw + 200.0f + static_cast<float>(c % 2) * 38.0f, -hh + 130.0f - static_cast<float>(c / 2) * 36.0f);
    }
    const char* game_buttons[] = {"b_retreat", "b_reinforcement", "b_special_weapon", "b_special_weapon2", "b_beacon_t",
                                  "b_camera_t"};
    for (int k = 0; k < 6; ++k) put(game_buttons[k], -hw + 20.0f + static_cast<float>(k) * 38.0f, -hh + 202.0f);
    // Land reinforcement pad r_RRCC: 5 rows x 4 columns from the top-left corner.
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 4; ++c) {
            char n[32];
            std::snprintf(n, sizeof n, "r_%02d%02d", r, c);
            put(n, -hw + 32.0f + static_cast<float>(c) * 43.0f, hh - 66.0f - static_cast<float>(r) * 46.0f);
        }
    put("Text_Credits_tactical", hw - 70.0f, hh - 16.0f);
    // FIXPASS_GALACTIC §4.2 bottom-left advisor frame: the i_tactical_controls.alo (FoC Models.meg) bone positions,
    // model units from the bottom-left corner: Text_Planet_tactical (179, 239), Movie_tactical (50, 288),
    // b_droid_help_tactical (19, 255; hit mesh -22..31 x -12..57), Text_tactical_help (65, 309). Capture C2: the "?" face is
    // centred on the help bone.
    put("Text_Planet_tactical", -hw + 179.0f, -hh + 239.0f);
    put("Movie_tactical", -hw + 50.0f, -hh + 288.0f);
    put("b_droid_help_tactical", -hw + 19.0f, -hh + 255.0f);
    put("Text_tactical_help", -hw + 65.0f, -hh + 309.0f);
    put("b_option_t", -hw + 20.0f, hh - 20.0f);
    put("b_play_pause_t", -hw + 56.0f, hh - 20.0f);
    put("b_fast_forward_t", -hw + 92.0f, hh - 20.0f);
}

}  // namespace alamo::ui
