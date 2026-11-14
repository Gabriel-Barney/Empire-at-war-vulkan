#include "alamo/anim/anim_events.hpp"

#include "alamo/core/ascii.hpp"

namespace alamo::anim {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> f;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (is_space(line[i]) || line[i] == ',')) ++i;
        const size_t b = i;
        while (i < line.size() && !is_space(line[i]) && line[i] != ',') ++i;
        if (i > b) f.push_back(line.substr(b, i - b));
    }
    return f;
}

bool parse_ms(std::string_view s, uint32_t& out) {
    if (s.empty() || s.size() > 9) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<uint32_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

std::string clip_stem(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos) path = path.substr(slash + 1);
    if (path.size() > 4) {
        const std::string ext = ascii_upper(path.substr(path.size() - 4));
        if (ext == ".ALA") path = path.substr(0, path.size() - 4);
    }
    return ascii_upper(path);
}

size_t AnimEventMap::parse(std::string_view text, std::vector<std::string>* diagnostics) {
    size_t added = 0, pos = 0;
    uint32_t line_no = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        const std::vector<std::string_view> f = fields(line);
        if (f.empty() || f[0][0] == ';' || f[0][0] == '#' || f[0].substr(0, 2) == "//") continue;
        const std::string k = ascii_upper(f[0]);
        const bool sound = k == "SOUND", surface = k == "SURFACE";
        AnimEventDef e;
        if (!((sound && f.size() >= 4) || (surface && f.size() >= 5)) || !parse_ms(f[2], e.time_ms)) {
            if (diagnostics) diagnostics->push_back("AnimationSFXMaps.txt(" + std::to_string(line_no) + "): bad line '" + std::string(line) + "'");
            continue;
        }
        e.kind = sound ? AnimEventDef::Kind::Sound : AnimEventDef::Kind::Surface;
        e.name = std::string(f[3]);
        if (surface) e.bone = std::string(f[4]);
        std::vector<AnimEventDef>& list = by_clip_[clip_stem(f[1])];
        auto it = list.end();  // time order, equal times keep file order
        while (it != list.begin() && (it - 1)->time_ms > e.time_ms) --it;
        list.insert(it, std::move(e));
        ++added;
    }
    rows_ += added;
    return added;
}

const std::vector<AnimEventDef>* AnimEventMap::find(std::string_view clip) const {
    const auto it = by_clip_.find(clip_stem(clip));
    return it == by_clip_.end() ? nullptr : &it->second;
}

void crossed_events(const std::vector<AnimEventDef>& events, float fps, uint32_t count, float before, float after, bool wrapped,
                    std::vector<uint32_t>& out) {
    if (!(fps > 0.0f) || count < 2) return;
    const float to_ms = 1000.0f / fps;
    const float b = before * to_ms, a = after * to_ms;
    const float end = static_cast<float>(count - 1) * to_ms;
    const auto range = [&](float lo, float hi) {
        for (size_t i = 0; i < events.size(); ++i) {
            const float t = static_cast<float>(events[i].time_ms);
            if (t >= lo && t < hi) out.push_back(static_cast<uint32_t>(i));
        }
    };
    if (wrapped) {
        range(b, end);  // the tail of the cycle first, then the new cycle
        range(0.0f, a);
    } else {
        range(b, a);
    }
}

}  // namespace alamo::anim
