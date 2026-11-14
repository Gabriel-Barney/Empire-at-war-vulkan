#include "alamo/video/movie_db.hpp"

#include <cmath>
#include <string>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::video {

namespace {
bool flag(const data::XmlNode& m, std::string_view tag, bool fallback) {
    const std::string_view t = m.child_text(tag);
    if (t.empty()) return fallback;
    if (const auto b = data::parse_bool(t)) return *b;
    return fallback;
}
std::string_view stem(std::string_view path) {
    const size_t s = path.find_last_of("\\/");
    if (s != std::string_view::npos) path.remove_prefix(s + 1);
    const size_t d = path.find_last_of('.');
    if (d != std::string_view::npos) path = path.substr(0, d);
    return path;
}
}  // namespace

std::vector<MovieEntry> parse_movies(const data::XmlNode& root) {
    std::vector<MovieEntry> out;
    for (data::XmlNode m : root.children("Movie")) {
        MovieEntry e;
        e.name = std::string(m.attribute("Name"));
        e.file = std::string(m.child_text("Movie_File"));
        e.alpha = flag(m, "Alpha", false);
        e.cannot_skip = flag(m, "Cannot_Skip", false);
        e.loop = flag(m, "Loop", false);
        float off[2] = {0, 0};
        if (data::parse_floats(m.child_text("Commandbar_Offset"), off, 2) == 2) {
            e.commandbar_offset[0] = static_cast<int>(std::lround(off[0]));
            e.commandbar_offset[1] = static_cast<int>(std::lround(off[1]));
        }
        out.push_back(std::move(e));
    }
    return out;
}

const MovieEntry* find_movie(const std::vector<MovieEntry>& movies, std::string_view key) {
    const MovieEntry* by_file = nullptr;
    for (size_t i = movies.size(); i-- > 0;) {
        if (ascii_iequals(movies[i].name, key)) return &movies[i];  // exact ASCII fold: `key` may be a path
        if (!by_file && !movies[i].file.empty() && ascii_iequals(stem(movies[i].file), stem(key))) by_file = &movies[i];
    }
    return by_file;
}

}  // namespace alamo::video
