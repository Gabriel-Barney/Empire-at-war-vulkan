#include "alamo/game/story_script.hpp"

#include <algorithm>
#include <cstddef>

#include "alamo/core/file_provider.hpp"

namespace alamo::game {

namespace {

// GALACTIC_DIALOGS §2.6: the whole file is one token stream; delimiters are space, tab, CR, LF and ']'. A token that
// starts with '#' skips the rest of its line.
class Tokens {
public:
    explicit Tokens(std::string_view s) : s_(s) {}
    // The next token, "" at the end of the file.
    std::string_view next() {
        for (;;) {
            while (pos_ < s_.size() && delim(s_[pos_])) ++pos_;
            if (pos_ >= s_.size()) return {};
            if (s_[pos_] == '#') {
                const size_t e = s_.find('\n', pos_);
                pos_ = e == std::string_view::npos ? s_.size() : e;
                continue;
            }
            const size_t b = pos_;
            while (pos_ < s_.size() && !delim(s_[pos_])) ++pos_;
            return s_.substr(b, pos_ - b);
        }
    }

private:
    static bool delim(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ']'; }
    std::string_view s_;
    size_t pos_ = 0;
};

// atoi: optional sign, leading digits; 0 when there are none (saturated to int32).
int32_t atoi_sv(std::string_view s) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = std::min<int64_t>(v * 10 + (s[i] - '0'), int64_t{1} << 31);
    v = neg ? -v : v;
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}

std::string upper(std::string_view s) {
    std::string u(s);
    for (char& c : u)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return u;
}

// Keyword table 0xa2a1d0 (§2.6): argument kinds 0 none, 1 one integer, 3 one string token, 4 four integers.
enum class Arg : uint8_t { None, Int, String, Int4 };
struct Keyword {
    const char* name;
    Arg arg;
};
constexpr Keyword kKeywords[] = {
    {"SFX", Arg::String},          {"MUSIC", Arg::String},         {"DIALOG", Arg::String},     {"MOVIE", Arg::String},
    {"MOVIE_ONCE", Arg::String},   {"ANIM", Arg::Int},             {"IMAGE", Arg::String},      {"TEXT", Arg::String},
    {"EXTRA_TEXT", Arg::None},     {"TITLE", Arg::String},         {"CLEARTEXT", Arg::None},    {"TEXTCOLOR", Arg::Int4},
    {"NEWLINE", Arg::Int},         {"WAIT", Arg::Int},             {"PAUSE", Arg::Int},         {"LOADCINEMATIC", Arg::String},
    {"CLEARIMAGE", Arg::None},     {"HIDECINEMATIC", Arg::Int},    {"STOPMOVIE", Arg::None},    {"WAIT_SPEECH", Arg::None},
    {"[CHAPTER", Arg::Int}};

}  // namespace

bool parse_story_script(std::string_view all, int32_t chapter, StoryScript& out) {
    out = StoryScript{};
    const int32_t want = chapter < 0 ? 0 : chapter;
    bool in = false, found = false;
    uint8_t color[4] = {0, 0, 0, 0};
    double now = 0.0;
    Tokens tk(all);
    auto add_line = [&](StoryScriptLine::Kind k, std::string_view id) {
        StoryScriptLine l;
        l.kind = k;
        l.text_id = std::string(id);
        std::copy(color, color + 4, l.rgba);
        l.at = now;
        out.lines.push_back(std::move(l));
    };
    for (std::string_view word = tk.next(); !word.empty(); word = tk.next()) {
        const std::string cmd = upper(word);
        const Keyword* kw = nullptr;
        for (const Keyword& k : kKeywords)
            if (cmd == k.name) kw = &k;
        if (!kw) {  // "Story script error!  Unrecognized keyword": the whole file is rejected
            out = StoryScript{};
            return false;
        }
        // Arguments are read whatever the chapter (the original parses the whole file).
        std::string_view str;
        int32_t n[4] = {0, 0, 0, 255};  // TEXTCOLOR: the 4th defaults to 255 when the file ends
        if (kw->arg == Arg::String) str = tk.next();
        else if (kw->arg == Arg::Int) n[0] = atoi_sv(tk.next());
        else if (kw->arg == Arg::Int4)
            for (int i = 0; i < 4; ++i) {
                const std::string_view t = tk.next();
                if (t.empty()) break;
                n[i] = atoi_sv(t);
            }
        if (cmd == "[CHAPTER") {
            in = n[0] == want;
            found = found || in;
            continue;
        }
        if (!in) continue;  // other chapters, and commands before the first chapter header
        if (cmd == "TITLE") {
            out.title = std::string(str);
        } else if (cmd == "TEXT") {
            add_line(StoryScriptLine::Kind::Text, str);
        } else if (cmd == "NEWLINE") {
            // [port] capped at 256 lines as a guard against malformed files; shipped scripts use 1 and 2.
            for (int32_t i = 0; i < std::clamp(n[0], 0, 256); ++i) add_line(StoryScriptLine::Kind::Blank, {});
        } else if (cmd == "TEXTCOLOR") {
            for (int i = 0; i < 4; ++i) color[i] = static_cast<uint8_t>(std::clamp(n[i], 0, 255));
        } else if (cmd == "WAIT") {
            if (n[0] > 0) now += static_cast<double>(n[0]) / 1000.0;
        } else if (cmd == "SFX" || cmd == "DIALOG") {
            if (!str.empty()) out.cues.push_back({cmd == "SFX" ? StoryScriptCue::Kind::Sfx : StoryScriptCue::Kind::Speech, std::string(str), now});
        } else if (cmd == "MOVIE" || cmd == "MOVIE_ONCE") {
            out.movie = std::string(str);
            out.movie_loop = cmd == "MOVIE";
        } else if (cmd == "IMAGE") {
            out.image = std::string(str);
        } else if (cmd == "EXTRA_TEXT") {
            add_line(StoryScriptLine::Kind::Extra, {});
        } else if (cmd == "PAUSE") {
            // GALACTIC_DIALOGS §2.6: PAUSE n is an on/off flag (n != 0 pause, 0 unpause), not a delay. The holocron summary
            // executor (0x1089e0), which every story event uses, has no case for it. IDD_STORY_DIALOG (0x1053c0) calls the
            // game mode's pause / unpause, which is an empty function in galactic mode, and that dialog only opens for
            // galactic random plots. So it has no effect in shipped play: counted, never acted on, the sim is not paused.
            ++out.pauses;
        }
        // MUSIC, ANIM, CLEARTEXT, LOADCINEMATIC, CLEARIMAGE, HIDECINEMATIC, STOPMOVIE, WAIT_SPEECH: recognised, no shipped
        // script uses them; skipped.
    }
    return found;
}

bool story_script_valid(std::string_view all) {
    Tokens tk(all);
    for (std::string_view word = tk.next(); !word.empty(); word = tk.next()) {
        const std::string cmd = upper(word);
        const Keyword* kw = nullptr;
        for (const Keyword& k : kKeywords)
            if (cmd == k.name) kw = &k;
        if (!kw) return false;
        if (kw->arg == Arg::String || kw->arg == Arg::Int) {
            tk.next();
        } else if (kw->arg == Arg::Int4) {
            for (int i = 0; i < 4; ++i)
                if (tk.next().empty()) break;
        }
    }
    return true;
}

bool StoryDialogCache::load(const FileProvider& files, std::string_view name) {
    if (name.empty() || cached(name)) return false;  // 0x47d9c0 returns 0 for a cached name
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\Scripts\\Story\\" + std::string(name) + ".txt", bytes)) return false;
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!story_script_valid(text)) return false;  // rejected: nothing cached
    files_.emplace(std::string(name), std::move(text));
    return true;
}

bool StoryDialogCache::chapter(std::string_view name, int32_t chapter, StoryScript& out) const {
    const auto it = files_.find(std::string(name));
    if (it == files_.end()) {
        out = StoryScript{};
        return false;
    }
    return parse_story_script(it->second, chapter, out);
}

HolocronSummaryFill fill_holocron_summary(const StoryDialogCache& cache, const std::vector<HolocronRecord>& records, bool completed_tab,
                                          uint64_t selected_id) {
    HolocronSummaryFill f;
    for (size_t i = 0; i < records.size(); ++i) {
        const HolocronRecord& r = records[i];
        f.has_completed = f.has_completed || r.completed;
        if (r.completed != completed_tab) continue;
        HolocronRow row;
        row.record = i;
        StoryScript sc;
        if (cache.chapter(r.dialog, r.chapter, sc))
            for (const StoryScriptLine& l : sc.lines) {
                if (l.kind != StoryScriptLine::Kind::Text) continue;
                row.text_id = l.text_id;
                if (l.rgba[3] != 0) std::copy(l.rgba, l.rgba + 4, row.rgba);  // else the default 128, 128, 255
                break;
            }
        if (r.read || r.completed)
            for (int k = 0; k < 3; ++k) row.rgba[k] = static_cast<uint8_t>(static_cast<float>(row.rgba[k]) * 0.66f);
        if (r.id == selected_id) f.selected = static_cast<int32_t>(f.rows.size());
        f.rows.push_back(std::move(row));
    }
    if (f.selected < 0 && !f.rows.empty()) f.selected = 0;
    if (f.selected < 0) return f;
    const HolocronRecord& r = records[f.rows[static_cast<size_t>(f.selected)].record];
    StoryScript sc;
    if (!cache.chapter(r.dialog, r.chapter, sc)) return f;
    f.title_id = sc.title;
    for (const StoryScriptLine& l : sc.lines) {
        if (l.kind == StoryScriptLine::Kind::Extra) {  // 0x1093b0: every stored line at once, in the current colour
            for (const std::string& e : r.lines) {
                HolocronRecapLine x;
                x.kind = StoryScriptLine::Kind::Extra;
                x.text = e;
                std::copy(l.rgba, l.rgba + 4, x.rgba);
                f.recap.push_back(std::move(x));
            }
            continue;
        }
        HolocronRecapLine x;
        x.kind = l.kind;
        x.text = l.text_id;
        std::copy(l.rgba, l.rgba + 4, x.rgba);
        f.recap.push_back(std::move(x));
    }
    return f;
}

std::u32string story_var_printf(std::u32string_view fmt, std::u32string_view var) {
    std::u32string out;
    bool used = false;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != U'%' || i + 1 >= fmt.size()) {
            out.push_back(fmt[i]);
            continue;
        }
        const char32_t n = fmt[i + 1];
        if (n == U'%') {
            out.push_back(U'%');
            ++i;
        } else if (n == U's' || n == U'S' || (n == U'l' && i + 2 < fmt.size() && fmt[i + 2] == U's')) {
            if (!used) out.append(var);
            used = true;
            i += n == U'l' ? 2 : 1;
        } else {
            out.push_back(fmt[i]);
        }
    }
    return out;
}

}  // namespace alamo::game
