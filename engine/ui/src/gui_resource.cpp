#include "alamo/ui/gui_resource.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::ui {

namespace {

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parse_int(std::string_view s, int32_t& out) {
    s = trim(s);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-' || s.front() == '+') {
        neg = s.front() == '-';
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    // Trailing L / U suffixes (Win32 headers).
    while (!s.empty() && (s.back() == 'L' || s.back() == 'l' || s.back() == 'U' || s.back() == 'u')) s.remove_suffix(1);
    if (s.empty()) return false;
    int64_t v = 0;
    for (char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return false;
        v = v * base + d;
        if (v > 0x7fffffffLL + 1) return false;
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

// ---- RC tokenizer --------------------------------------------------------------------------------------
struct Token {
    enum class Kind : uint8_t { Word, String, Comma, Pipe, End } kind = Kind::End;
    std::string text;
    uint32_t line = 0;
};

std::vector<Token> tokenize_rc(std::string_view s) {
    std::vector<Token> out;
    uint32_t line = 1;
    bool line_start = true;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            ++line;
            line_start = true;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (line_start && c == '#') {  // preprocessor line (#include, #if, #define, #pragma, ...)
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        line_start = false;
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') ++line;
                ++i;
            }
            i += 2;
            continue;
        }
        Token t;
        t.line = line;
        if (c == '"') {  // "" inside a string is a literal quote
            t.kind = Token::Kind::String;
            ++i;
            while (i < s.size()) {
                if (s[i] == '"') {
                    if (i + 1 < s.size() && s[i + 1] == '"') {
                        t.text.push_back('"');
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                if (s[i] == '\n') ++line;
                t.text.push_back(s[i++]);
            }
        } else if (c == ',') {
            t.kind = Token::Kind::Comma;
            ++i;
        } else if (c == '|') {
            t.kind = Token::Kind::Pipe;
            ++i;
        } else {
            t.kind = Token::Kind::Word;
            while (i < s.size()) {
                const char d = s[i];
                if (d == ',' || d == '|' || d == '"' || std::isspace(static_cast<unsigned char>(d))) break;
                t.text.push_back(d);
                ++i;
            }
            if (t.text.empty()) {  // stray character
                ++i;
                continue;
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

// One comma-separated argument: a single token, or a style expression (term | term | NOT term ...).
struct Arg {
    std::vector<std::string> terms;  // words / strings; "NOT X" -> "!X"
    bool is_string = false;
    std::string first() const { return terms.empty() ? std::string() : terms.front(); }
};

class RcParser {
public:
    explicit RcParser(std::vector<Token> t) : t_(std::move(t)) {}
    const Token& peek(size_t k = 0) const {
        static const Token kEnd{};
        return pos_ + k < t_.size() ? t_[pos_ + k] : kEnd;
    }
    bool at_end() const { return pos_ >= t_.size(); }
    void next() { ++pos_; }
    bool word_is(size_t k, std::string_view w) const { return peek(k).kind == Token::Kind::Word && iequal(peek(k).text, w); }

    Arg arg() {
        Arg a;
        bool want_term = true;
        while (!at_end()) {
            const Token& t = peek();
            if (want_term) {
                if (t.kind == Token::Kind::Word && iequal(t.text, "NOT") && peek(1).kind == Token::Kind::Word) {
                    a.terms.push_back("!" + peek(1).text);
                    pos_ += 2;
                } else if (t.kind == Token::Kind::Word || t.kind == Token::Kind::String) {
                    if (t.kind == Token::Kind::String) a.is_string = true;
                    a.terms.push_back(t.text);
                    next();
                } else {
                    break;
                }
                want_term = false;
            } else if (t.kind == Token::Kind::Pipe) {
                next();
                want_term = true;
            } else {
                break;
            }
        }
        return a;
    }
    // Comma-separated argument list (stops at a token that is not preceded by a comma).
    std::vector<Arg> args() {
        std::vector<Arg> out;
        out.push_back(arg());
        while (peek().kind == Token::Kind::Comma) {
            next();
            out.push_back(arg());
        }
        return out;
    }
    void skip_block() {  // positioned at BEGIN
        int depth = 0;
        while (!at_end()) {
            if (word_is(0, "BEGIN") || (peek().kind == Token::Kind::Word && peek().text == "{")) ++depth;
            if (word_is(0, "END") || (peek().kind == Token::Kind::Word && peek().text == "}")) {
                --depth;
                if (depth <= 0) {
                    next();
                    return;
                }
            }
            next();
        }
    }
    size_t pos_ = 0;

private:
    std::vector<Token> t_;
};

int32_t arg_int(const Arg& a) {
    int32_t v = 0;
    if (!a.terms.empty()) parse_int(a.terms.front(), v);
    return v;
}

RcControlKind control_kind(std::string_view kw) {
    if (iequal(kw, "PUSHBUTTON")) return RcControlKind::PushButton;
    if (iequal(kw, "DEFPUSHBUTTON")) return RcControlKind::DefPushButton;
    if (iequal(kw, "LTEXT")) return RcControlKind::LText;
    if (iequal(kw, "RTEXT")) return RcControlKind::RText;
    if (iequal(kw, "CTEXT")) return RcControlKind::CText;
    if (iequal(kw, "EDITTEXT")) return RcControlKind::EditText;
    if (iequal(kw, "COMBOBOX")) return RcControlKind::ComboBox;
    if (iequal(kw, "LISTBOX")) return RcControlKind::ListBox;
    if (iequal(kw, "GROUPBOX")) return RcControlKind::GroupBox;
    if (iequal(kw, "CONTROL")) return RcControlKind::Control;
    return RcControlKind::Other;
}

bool is_control_keyword(std::string_view kw) {
    static constexpr std::string_view kOther[] = {"AUTOCHECKBOX", "AUTORADIOBUTTON", "CHECKBOX", "RADIOBUTTON", "STATE3",
                                                  "AUTO3STATE",   "ICON",            "SCROLLBAR", "PUSHBOX"};
    if (control_kind(kw) != RcControlKind::Other) return true;
    for (std::string_view k : kOther)
        if (iequal(kw, k)) return true;
    return false;
}

void append_styles(std::vector<std::string>& out, const Arg& a) {
    for (const std::string& t : a.terms) out.push_back(t);
}

}  // namespace

// ---- ResourceIds -----------------------------------------------------------------------------------------
size_t ResourceIds::parse(std::string_view text) {
    size_t n = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = trim(text.substr(i, e - i));
        i = e + 1;
        if (line.substr(0, 7) != "#define") continue;
        line = trim(line.substr(7));
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, sp);
        std::string_view value = trim(line.substr(sp));
        const size_t cm = value.find("//");
        if (cm != std::string_view::npos) value = trim(value.substr(0, cm));
        int32_t v = 0;
        if (!parse_int(value, v)) continue;
        by_name_[std::string(name)] = v;
        by_id_.emplace(v, std::string(name));  // first name wins
        ++n;
    }
    return n;
}

std::optional<int32_t> ResourceIds::id(std::string_view name) const {
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) return std::nullopt;
    return it->second;
}

std::string_view ResourceIds::name(int32_t id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? std::string_view() : std::string_view(it->second);
}

// ---- RcControl / RcDialog ----------------------------------------------------------------------------------
const char* gadget_type_name(GuiGadgetType t) {
    switch (t) {
        case GuiGadgetType::PushButton: return "PushButton";
        case GuiGadgetType::Text: return "Text";
        case GuiGadgetType::EditBox: return "EditBox";
        case GuiGadgetType::IMEEditBox: return "IMEEditBox";
        case GuiGadgetType::ComboBox: return "ComboBox";
        case GuiGadgetType::ListBox: return "ListBox";
        case GuiGadgetType::CheckBox: return "CheckBox";
        case GuiGadgetType::RadioButton: return "RadioButton";
        case GuiGadgetType::TrackbarSlider: return "TrackbarSlider";
        case GuiGadgetType::ProgressBar: return "ProgressBar";
        case GuiGadgetType::VScrollbar: return "VScrollbar";
        case GuiGadgetType::CustomImage: return "CustomImage";
        case GuiGadgetType::Frame: return "Frame";
        case GuiGadgetType::IMELocaleIndicator: return "IMELocaleIndicator";
        default: return "Unknown";
    }
}

bool RcControl::has_style(std::string_view s) const {
    for (const std::string& t : styles)
        if (iequal(t, s)) return true;
    return false;
}

bool RcDialog::has_style(std::string_view s) const {
    for (const std::string& t : styles)
        if (iequal(t, s)) return true;
    return false;
}

int32_t RcDialog::find(std::string_view id) const {
    for (size_t i = 0; i < controls.size(); ++i)
        if (iequal(controls[i].id, id)) return static_cast<int32_t>(i);
    return -1;
}

GuiGadgetType gadget_type_of(const RcControl& c) {
    switch (c.kind) {
        case RcControlKind::PushButton:
        case RcControlKind::DefPushButton: return GuiGadgetType::PushButton;
        case RcControlKind::LText:
        case RcControlKind::RText:
        case RcControlKind::CText: return GuiGadgetType::Text;
        case RcControlKind::EditText: return GuiGadgetType::EditBox;
        case RcControlKind::ComboBox: return GuiGadgetType::ComboBox;
        case RcControlKind::ListBox: return GuiGadgetType::ListBox;
        case RcControlKind::GroupBox: return GuiGadgetType::Frame;
        default: break;
    }
    const std::string& k = c.keyword;
    if (iequal(k, "AUTOCHECKBOX") || iequal(k, "CHECKBOX") || iequal(k, "STATE3") || iequal(k, "AUTO3STATE"))
        return GuiGadgetType::CheckBox;
    if (iequal(k, "AUTORADIOBUTTON") || iequal(k, "RADIOBUTTON")) return GuiGadgetType::RadioButton;
    if (iequal(k, "ICON")) return GuiGadgetType::CustomImage;
    if (iequal(k, "SCROLLBAR")) return GuiGadgetType::VScrollbar;
    const std::string& cls = c.class_name;
    if (iequal(cls, "Button")) {
        if (c.has_style("BS_AUTOCHECKBOX") || c.has_style("BS_CHECKBOX") || c.has_style("BS_3STATE") ||
            c.has_style("BS_AUTO3STATE"))
            return GuiGadgetType::CheckBox;
        if (c.has_style("BS_AUTORADIOBUTTON") || c.has_style("BS_RADIOBUTTON")) return GuiGadgetType::RadioButton;
        if (c.has_style("BS_GROUPBOX")) return GuiGadgetType::Frame;
        return GuiGadgetType::PushButton;
    }
    if (iequal(cls, "msctls_trackbar32")) return GuiGadgetType::TrackbarSlider;
    if (iequal(cls, "msctls_progress32")) return GuiGadgetType::ProgressBar;
    if (iequal(cls, "PETROGLYPH_DIALOG_IMAGE")) return GuiGadgetType::CustomImage;
    if (iequal(cls, "IMEEditBox")) return GuiGadgetType::IMEEditBox;
    if (iequal(cls, "IMELocaleIndicator")) return GuiGadgetType::IMELocaleIndicator;
    if (iequal(cls, "Static")) return GuiGadgetType::Text;
    if (iequal(cls, "Edit")) return GuiGadgetType::EditBox;
    if (iequal(cls, "ListBox")) return GuiGadgetType::ListBox;
    if (iequal(cls, "ComboBox")) return GuiGadgetType::ComboBox;
    if (iequal(cls, "ScrollBar")) return GuiGadgetType::VScrollbar;
    return GuiGadgetType::Unknown;
}

void RcControl::classify() {
    gadget = gadget_type_of(*this);
    default_button = kind == RcControlKind::DefPushButton || has_style("BS_DEFPUSHBUTTON");
    pushlike = has_style("BS_PUSHLIKE");
    visible = !has_style("!WS_VISIBLE");
    disabled = has_style("WS_DISABLED");
    group = has_style("WS_GROUP");
    align = TextAlign::Left;
    if (kind == RcControlKind::RText || has_style("SS_RIGHT")) align = TextAlign::Right;
    if (kind == RcControlKind::CText || has_style("SS_CENTER")) align = TextAlign::Center;
    const bool default_tab = kind == RcControlKind::PushButton || kind == RcControlKind::DefPushButton ||
                             kind == RcControlKind::EditText;
    tabstop = (default_tab || has_style("WS_TABSTOP")) && !has_style("!WS_TABSTOP");
    // [port] texts, images, frames, progress bars and locale indicators never take focus (the data sets WS_TABSTOP on
    // 718 PETROGLYPH_DIALOG_IMAGE controls).
    switch (gadget) {
        case GuiGadgetType::Text:
        case GuiGadgetType::CustomImage:
        case GuiGadgetType::Frame:
        case GuiGadgetType::ProgressBar:
        case GuiGadgetType::IMELocaleIndicator:
        case GuiGadgetType::Unknown: tabstop = false; break;
        default: break;
    }
}

// ---- DialogScript -------------------------------------------------------------------------------------------
bool DialogScript::parse(std::string_view text, std::string* error) {
    dialogs_.clear();
    index_.clear();
    warnings.clear();
    RcParser p(tokenize_rc(text));
    while (!p.at_end()) {
        if (p.word_is(0, "BEGIN") || (p.peek().kind == Token::Kind::Word && p.peek().text == "{")) {
            p.skip_block();
            continue;
        }
        if (p.peek().kind == Token::Kind::Word && (p.word_is(1, "DIALOGEX") || p.word_is(1, "DIALOG"))) {
            RcDialog d;
            d.name = p.peek().text;
            d.line = p.peek().line;
            p.next();
            p.next();
            const std::vector<Arg> rect = p.args();
            if (rect.size() >= 4) {
                d.x = arg_int(rect[0]);
                d.y = arg_int(rect[1]);
                d.w = arg_int(rect[2]);
                d.h = arg_int(rect[3]);
            }
            // Header statements until BEGIN.
            while (!p.at_end() && !p.word_is(0, "BEGIN") && !(p.peek().kind == Token::Kind::Word && p.peek().text == "{")) {
                if (p.word_is(0, "STYLE") || p.word_is(0, "EXSTYLE")) {
                    p.next();
                    append_styles(d.styles, p.arg());
                } else if (p.word_is(0, "CAPTION")) {
                    p.next();
                    d.caption = p.arg().first();
                } else if (p.word_is(0, "FONT")) {
                    p.next();
                    const std::vector<Arg> f = p.args();
                    if (!f.empty()) d.font_size = arg_int(f[0]);
                    if (f.size() > 1) d.font_face = f[1].first();
                } else {
                    p.next();  // MENU, CLASS, LANGUAGE, CHARACTERISTICS, VERSION ... (one token argument)
                }
            }
            if (p.at_end()) break;
            p.next();  // BEGIN
            while (!p.at_end() && !p.word_is(0, "END") && !(p.peek().kind == Token::Kind::Word && p.peek().text == "}")) {
                const Token& kw = p.peek();
                if (kw.kind != Token::Kind::Word || !is_control_keyword(kw.text)) {
                    warnings.push_back(d.name + ": unexpected token '" + kw.text + "' at line " + std::to_string(kw.line));
                    p.next();
                    continue;
                }
                RcControl c;
                c.keyword = upper(kw.text);
                c.kind = control_kind(kw.text);
                c.line = kw.line;
                p.next();
                const std::vector<Arg> a = p.args();
                auto num = [&](size_t i) { return i < a.size() ? arg_int(a[i]) : 0; };
                size_t k = 0;
                const bool no_text = c.kind == RcControlKind::EditText || c.kind == RcControlKind::ComboBox ||
                                     c.kind == RcControlKind::ListBox || iequal(c.keyword, "SCROLLBAR");
                if (!no_text && k < a.size()) c.caption = a[k++].first();
                if (k < a.size()) c.id = a[k++].first();
                if (c.kind == RcControlKind::Control) {
                    if (k < a.size()) c.class_name = a[k++].first();
                    if (k < a.size()) append_styles(c.styles, a[k++]);
                }
                c.x = num(k);
                c.y = num(k + 1);
                c.w = num(k + 2);
                c.h = num(k + 3);
                k += 4;
                for (; k < a.size(); ++k) append_styles(c.styles, a[k]);  // style [, exstyle]
                if (a.size() < 5) warnings.push_back(d.name + ": short control statement at line " + std::to_string(c.line));
                c.classify();
                d.controls.push_back(std::move(c));
            }
            p.next();  // END
            index_.emplace(upper(d.name), static_cast<uint32_t>(dialogs_.size()));
            dialogs_.push_back(std::move(d));
            continue;
        }
        p.next();
    }
    if (dialogs_.empty()) {
        if (error) *error = "GUIDialogs.rc: no DIALOGEX blocks";
        return false;
    }
    return true;
}

const RcDialog* DialogScript::find(std::string_view name) const {
    const auto it = index_.find(upper(name));
    return it == index_.end() ? nullptr : &dialogs_[it->second];
}

bool DialogScript::merge(std::string_view text, std::string* error) {
    DialogScript more;
    if (!more.parse(text, error)) return false;
    for (RcDialog& d : more.dialogs_) {
        const auto it = index_.find(upper(d.name));
        if (it != index_.end()) {
            dialogs_[it->second] = std::move(d);
        } else {
            index_.emplace(upper(d.name), static_cast<uint32_t>(dialogs_.size()));
            dialogs_.push_back(std::move(d));
        }
    }
    warnings.insert(warnings.end(), more.warnings.begin(), more.warnings.end());
    return true;
}

bool DialogScript::extend(std::string_view text, std::string* error) {
    DialogScript more;
    if (!more.parse(text, error)) return false;
    for (RcDialog& d : more.dialogs_) {
        const auto it = index_.find(upper(d.name));
        if (it != index_.end()) {
            std::vector<RcControl>& c = dialogs_[it->second].controls;
            c.insert(c.end(), std::make_move_iterator(d.controls.begin()), std::make_move_iterator(d.controls.end()));
        } else {
            index_.emplace(upper(d.name), static_cast<uint32_t>(dialogs_.size()));
            dialogs_.push_back(std::move(d));
        }
    }
    warnings.insert(warnings.end(), more.warnings.begin(), more.warnings.end());
    return true;
}

size_t DialogScript::control_count() const {
    size_t n = 0;
    for (const RcDialog& d : dialogs_) n += d.controls.size();
    return n;
}

// ---- GuiSkin ------------------------------------------------------------------------------------------------
std::string_view gadget_font_key(GuiGadgetType type, TextAlign align) {
    switch (type) {
        case GuiGadgetType::PushButton:
        case GuiGadgetType::CheckBox:
        case GuiGadgetType::RadioButton: return "PUSH_BUTTON";  // [port] check / radio captions use the button font
        case GuiGadgetType::ListBox: return "LIST_BOX";
        case GuiGadgetType::ComboBox: return "COMBO_BOX";
        case GuiGadgetType::EditBox: return "EDIT_BOX";
        case GuiGadgetType::IMEEditBox: return "IME_EDIT_BOX";
        case GuiGadgetType::Text:
        case GuiGadgetType::Frame:
            return align == TextAlign::Right ? "R_TEXT" : align == TextAlign::Center ? "C_TEXT" : "L_TEXT";
        default: return {};
    }
}

namespace {
Rgba8 parse_color(std::string_view s, Rgba8 fallback) {
    int32_t v[4] = {fallback.r, fallback.g, fallback.b, fallback.a};
    size_t n = 0, i = 0;
    while (i < s.size() && n < 4) {
        while (i < s.size() && !(std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-')) ++i;
        size_t e = i;
        while (e < s.size() && (std::isdigit(static_cast<unsigned char>(s[e])) || s[e] == '-')) ++e;
        if (e == i) break;
        int32_t x = 0;
        if (parse_int(s.substr(i, e - i), x)) v[n++] = std::clamp(x, 0, 255);
        i = e;
    }
    return {static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2]), static_cast<uint8_t>(v[3])};
}

bool yes(std::string_view s) {
    s = trim(s);
    return iequal(s, "yes") || iequal(s, "true") || s == "1";
}

bool is_font_entry(const data::XmlNode& n) { return static_cast<bool>(n.child("Name")) || static_cast<bool>(n.child("Size")); }

GuiFontSpec read_font(const data::XmlNode& n, const GuiFontSpec& base) {
    GuiFontSpec f = base;
    if (const auto v = n.child_text("Name"); !v.empty()) f.face = std::string(v);
    if (const auto v = n.child_text("Size"); !v.empty()) f.size = static_cast<float>(std::atof(std::string(v).c_str()));
    if (const auto v = n.child_text("Character_Padding"); !v.empty())
        f.padding = static_cast<float>(std::atof(std::string(v).c_str()));
    if (const auto v = n.child_text("Stretch_Factor"); !v.empty())
        f.stretch = static_cast<float>(std::atof(std::string(v).c_str()));
    if (f.stretch <= 0.0f) f.stretch = 1.0f;
    if (const auto v = n.child_text("Top_Color"); !v.empty()) f.top = parse_color(v, f.top);
    if (const auto v = n.child_text("Bottom_Color"); !v.empty()) f.bottom = parse_color(v, f.bottom);
    if (n.child("Emboss")) f.emboss = yes(n.child_text("Emboss"));
    if (n.child("Outline")) f.outline = yes(n.child_text("Outline"));
    return f;
}
}  // namespace

bool GuiSkin::parse(const data::XmlNode& root) {
    default_.clear();
    overrides_.clear();
    default_fonts_.clear();
    control_fonts_.clear();
    dialog_fonts_.clear();
    tooltips_.clear();
    font_entries_ = 0;
    global_ = GuiFontSpec{};
    const data::XmlNode tex = root.child("Textures");
    if (tex) {
        if (const auto f = tex.attribute("File"); !f.empty()) atlas_file = std::string(f);
        compressed_atlas_file = std::string(tex.attribute("Compressed_File"));
        for (data::XmlNode set = tex.first_child(); set; set = set.next_sibling()) {
            const std::string name = upper(set.name());
            KeyMap& km = name == "DEFAULT" ? default_ : overrides_[name];  // duplicate sets merge, later keys win [port]
            for (data::XmlNode k = set.first_child(); k; k = k.next_sibling()) {
                std::string v(trim(k.text()));
                if (iequal(v, "none")) v.clear();
                km[upper(k.name())] = std::move(v);
            }
        }
    }
    const data::XmlNode fonts = root.child("Fonts");
    if (fonts) {
        const data::XmlNode def = fonts.child("Default");
        if (def) {
            if (const data::XmlNode g = def.child("Global_Default")) global_ = read_font(g, global_);
            for (data::XmlNode e = def.first_child(); e; e = e.next_sibling()) {
                if (!is_font_entry(e)) continue;
                default_fonts_[upper(e.name())] = read_font(e, global_);
                ++font_entries_;
            }
        }
        for (data::XmlNode e = fonts.first_child(); e; e = e.next_sibling()) {
            const std::string name = upper(e.name());
            if (name == "DEFAULT") continue;
            if (is_font_entry(e)) {
                control_fonts_[name] = read_font(e, global_);
                ++font_entries_;
            } else {  // <IDD_...> with nested gadget-key / IDC entries
                auto& m = dialog_fonts_[name];
                for (data::XmlNode n = e.first_child(); n; n = n.next_sibling()) {
                    if (!is_font_entry(n)) continue;
                    m[upper(n.name())] = read_font(n, global_);
                    ++font_entries_;
                }
            }
        }
    }
    if (const data::XmlNode tips = root.child("Tooltips")) {
        for (data::XmlNode e = tips.first_child(); e; e = e.next_sibling()) {
            const auto key = e.child_text("TextID");
            if (!key.empty()) tooltips_[upper(e.name())] = std::string(key);
        }
    }
    return static_cast<bool>(tex) || static_cast<bool>(fonts);
}

std::string_view GuiSkin::texture(std::string_view dialog, std::string_view control, std::string_view key) const {
    const std::string k = upper(key);
    for (std::string_view owner : {control, dialog}) {
        if (owner.empty()) continue;
        const auto it = overrides_.find(upper(owner));
        if (it == overrides_.end()) continue;
        const auto kt = it->second.find(k);
        if (kt != it->second.end()) return kt->second;
    }
    const auto it = default_.find(k);
    return it == default_.end() ? std::string_view() : std::string_view(it->second);
}

bool GuiSkin::has_override(std::string_view owner) const { return overrides_.count(upper(owner)) != 0; }

const GuiFontSpec& GuiSkin::font(std::string_view dialog, std::string_view control, GuiGadgetType type,
                                 TextAlign align) const {
    const std::string d = upper(dialog), c = upper(control), g(gadget_font_key(type, align));
    const auto dit = dialog_fonts_.find(d);
    if (dit != dialog_fonts_.end() && !c.empty())
        if (const auto it = dit->second.find(c); it != dit->second.end()) return it->second;
    if (!c.empty())
        if (const auto it = control_fonts_.find(c); it != control_fonts_.end()) return it->second;
    if (!g.empty()) {
        if (dit != dialog_fonts_.end())
            if (const auto it = dit->second.find(g); it != dit->second.end()) return it->second;
        if (const auto it = default_fonts_.find(g); it != default_fonts_.end()) return it->second;
    }
    return global_;
}

std::string_view GuiSkin::tooltip(std::string_view control) const {
    const auto it = tooltips_.find(upper(control));
    return it == tooltips_.end() ? std::string_view() : std::string_view(it->second);
}

bool load_gui_data(const FileProvider& files, GuiData& out, std::string* error) {
    std::string err;
    std::vector<uint8_t> bytes;
    bool ok = true;
    if (files.read(kGuiDialogsRcPath, bytes)) {
        std::string e;
        if (!out.script.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &e)) {
            err += e + "; ";
            ok = false;
        }
    } else {
        err += std::string(kGuiDialogsRcPath) + " missing; ";
        ok = false;
    }
    bytes.clear();
    if (files.read(kGuiResourceHPath, bytes))
        out.ids.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    else
        err += std::string(kGuiResourceHPath) + " missing; ";
    bytes.clear();
    if (files.read(kGuiDialogsXmlPath, bytes)) {
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, kGuiDialogsXmlPath);
        if (!doc.ok() || !out.skin.parse(doc.root())) {
            err += "GUIDialogs.xml unparsable; ";
            ok = false;
        }
    } else {
        err += std::string(kGuiDialogsXmlPath) + " missing; ";
        ok = false;
    }
    if (error) *error = err;
    return ok;
}

}  // namespace alamo::ui
