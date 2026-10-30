// Weather / map ambience / faction audio tables (see alamo/audio/game_audio_data.hpp).
#include "alamo/audio/game_audio_data.hpp"

#include <cstdio>
#include <optional>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::audio {
namespace {

using data::XmlDocument;
using data::XmlNode;

std::string event_or_none(std::string_view s) {
    s = data::trim(s);
    return iequals(s, "None") ? std::string() : std::string(s);
}

bool contains_ci(std::string_view s, std::string_view needle) {
    if (needle.size() > s.size()) return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i)
        if (iequals(s.substr(i, needle.size()), needle)) return true;
    return false;
}

void diag(GameAudioLoadReport& r, std::string_view file, uint32_t line, const std::string& msg) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "(%u): ", line);
    r.diagnostics.push_back(std::string(file) + buf + msg);
}

std::optional<float> field_float(const std::vector<std::string_view>& f, size_t i) {
    if (i >= f.size()) return std::nullopt;
    return data::parse_float(f[i]);
}

bool read_text(const FileProvider& files, std::string_view path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

}  // namespace

// ---------------------------------------------------------------- lookups

const WeatherLoopEntry* WeatherAudioTable::loop_for(std::string_view weather, float intensity) const {
    const WeatherLoopEntry* best = nullptr;
    for (const WeatherLoopEntry& e : loops)
        if (iequals(e.weather, weather) && e.intensity <= intensity && (!best || e.intensity > best->intensity))
            best = &e;
    return best;
}

const WeatherIntermittentEntry* WeatherAudioTable::intermittent_for(std::string_view weather, float intensity) const {
    const WeatherIntermittentEntry* best = nullptr;
    for (const WeatherIntermittentEntry& e : intermittents)
        if (iequals(e.weather, weather) && e.intensity <= intensity && (!best || e.intensity > best->intensity))
            best = &e;
    return best;
}

const AmbientIntermittentEntry* WeatherAudioTable::ambient_for(std::string_view environment) const {
    for (const AmbientIntermittentEntry& e : ambient)
        if (iequals(e.environment, environment)) return &e;
    return nullptr;
}

std::string_view FactionAudio::get(std::string_view tag) const {
    std::string_view v;
    for (const auto& [t, text] : fields)
        if (iequals(t, tag)) v = text;
    return v;
}

std::vector<std::string> FactionAudio::pair_values(std::string_view tag, std::string_view key) const {
    std::vector<std::string> out;
    for (const auto& [t, text] : fields) {
        if (!iequals(t, tag)) continue;
        const std::vector<std::string_view> f = data::split_fields(text);
        if (f.size() >= 2 && iequals(f[0], key) && !f[1].empty()) out.emplace_back(f[1]);
    }
    return out;
}

const FactionAudio* GameAudioData::find_faction(std::string_view name) const {
    for (const FactionAudio& f : factions_)
        if (iequals(f.name, name)) return &f;
    return nullptr;
}

const AmbientMapSound* GameAudioData::find_ambient_map_sound(std::string_view name) const {
    for (const AmbientMapSound& a : ambient_)
        if (iequals(a.name, name)) return &a;
    return nullptr;
}

// ---------------------------------------------------------------- AnimationSFXMaps.txt

void AnimationSfxMap::parse(std::string_view text, std::string_view file, std::vector<std::string>* diagnostics) {
    uint32_t line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        line = data::trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line.substr(0, 2) == "//") {
            if (end == text.size()) break;
            continue;
        }
        const std::vector<std::string_view> f = data::split_list(line);
        const bool sound = !f.empty() && iequals(f[0], "SOUND");
        const bool surface = !f.empty() && iequals(f[0], "SURFACE");
        const std::optional<int32_t> t = f.size() > 2 ? data::parse_int(f[2]) : std::nullopt;
        if (!(sound && f.size() >= 4) && !(surface && f.size() >= 5)) {
            if (diagnostics) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "(%u): ", line_no);
                diagnostics->push_back(std::string(file) + buf + "bad line '" + std::string(line) + "'");
            }
        } else if (!t || *t < 0) {
            if (diagnostics) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "(%u): ", line_no);
                diagnostics->push_back(std::string(file) + buf + "bad time '" + std::string(f[2]) + "'");
            }
        } else {
            AnimSfxEntry e;
            e.kind = sound ? AnimSfxEntry::Kind::Sound : AnimSfxEntry::Kind::Surface;
            e.time_ms = static_cast<uint32_t>(*t);
            e.name = std::string(f[3]);
            if (surface) e.bone = std::string(f[4]);
            std::vector<AnimSfxEntry>& list = by_anim_[normalize_name(f[1])];
            // Keep time order; equal times keep file order.
            auto it = list.end();
            while (it != list.begin() && (it - 1)->time_ms > e.time_ms) --it;
            list.insert(it, std::move(e));
            ++entries_;
        }
        if (end == text.size()) break;
    }
}

const std::vector<AnimSfxEntry>* AnimationSfxMap::entries(std::string_view animation) const {
    auto it = by_anim_.find(normalize_name(animation));
    return it == by_anim_.end() ? nullptr : &it->second;
}

std::vector<const AnimSfxEntry*> AnimationSfxMap::crossed(std::string_view animation, int64_t prev_ms,
                                                          int64_t now_ms) const {
    std::vector<const AnimSfxEntry*> out;
    const std::vector<AnimSfxEntry>* list = entries(animation);
    if (!list) return out;
    for (const AnimSfxEntry& e : *list) {
        const int64_t t = static_cast<int64_t>(e.time_ms);
        if (t > prev_ms && t <= now_ms) out.push_back(&e);
    }
    return out;
}

// ---------------------------------------------------------------- parsing

void GameAudioData::parse_weather_audio_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r) {
    XmlDocument doc = XmlDocument::parse(xml, std::string(file));
    if (!doc.ok()) {
        diag(r, file, 0, "unparsable");
        return;
    }
    for (XmlNode n : doc.root().children()) {
        const std::vector<std::string_view> f = data::split_fields(n.text_trimmed());
        if (iequals(n.name(), "Weather_SFXEvent_Loop")) {
            const auto in = field_float(f, 1);
            if (f.size() < 3 || !in) {
                diag(r, file, n.line(), "bad Weather_SFXEvent_Loop '" + std::string(n.text_trimmed()) + "'");
                continue;
            }
            weather_.loops.push_back(WeatherLoopEntry{std::string(f[0]), *in, event_or_none(f[2])});
            ++r.weather_loops;
        } else if (iequals(n.name(), "Weather_SFXEvent_Intermittent")) {
            const auto in = field_float(f, 1), lo = field_float(f, 3), hi = field_float(f, 4);
            if (f.size() < 5 || !in || !lo || !hi) {
                diag(r, file, n.line(), "bad Weather_SFXEvent_Intermittent '" + std::string(n.text_trimmed()) + "'");
                continue;
            }
            weather_.intermittents.push_back(
                WeatherIntermittentEntry{std::string(f[0]), *in, event_or_none(f[2]), *lo, *hi});
            ++r.weather_intermittents;
        } else if (iequals(n.name(), "Ambient_SFXEvent_Intermittent")) {
            const auto lo = field_float(f, 2), hi = field_float(f, 3);
            if (f.size() < 4 || !lo || !hi) {
                diag(r, file, n.line(), "bad Ambient_SFXEvent_Intermittent '" + std::string(n.text_trimmed()) + "'");
                continue;
            }
            weather_.ambient.push_back(AmbientIntermittentEntry{std::string(f[0]), event_or_none(f[1]), *lo, *hi});
            ++r.ambient_intermittents;
        } else {
            diag(r, file, n.line(), "unknown tag " + std::string(n.name()));
        }
    }
}

void GameAudioData::parse_ambient_map_sounds_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r) {
    XmlDocument doc = XmlDocument::parse(xml, std::string(file));
    if (!doc.ok()) {
        diag(r, file, 0, "unparsable");
        return;
    }
    for (XmlNode n : doc.root().children()) {
        // The shipped file uses both <AmbientMapSound> and <Ambient_Map_Sound>.
        if (!iequals(n.name(), "AmbientMapSound") && !iequals(n.name(), "Ambient_Map_Sound")) continue;
        AmbientMapSound a;
        a.name = std::string(data::trim(n.attribute("Name")));
        for (XmlNode c : n.children("Ambient")) {
            const std::vector<std::string_view> f = data::split_fields(c.text_trimmed());
            const auto d0 = field_float(f, 1), d1 = field_float(f, 2), s0 = field_float(f, 3), s1 = field_float(f, 4);
            if (f.size() < 5 || !d0 || !d1 || !s0 || !s1) {
                diag(r, file, c.line(), "bad Ambient '" + std::string(c.text_trimmed()) + "'");
                continue;
            }
            a.entries.push_back(AmbientMapSoundEntry{std::string(f[0]), *d0, *d1, *s0, *s1});
        }
        if (find_ambient_map_sound(a.name)) {
            diag(r, file, n.line(), "duplicate AmbientMapSound " + a.name + " (first kept)");
            continue;
        }
        ambient_.push_back(std::move(a));
        ++r.ambient_map_sounds;
    }
}

void GameAudioData::parse_factions_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r) {
    XmlDocument doc = XmlDocument::parse(xml, std::string(file));
    if (!doc.ok()) {
        diag(r, file, 0, "unparsable");
        return;
    }
    for (XmlNode n : doc.root().children("Faction")) {
        FactionAudio f;
        f.name = std::string(data::trim(n.attribute("Name")));
        for (XmlNode c : n.children()) {
            const std::string_view tag = c.name();
            if (contains_ci(tag, "SFX") || contains_ci(tag, "Music") || contains_ci(tag, "Speech"))
                f.fields.emplace_back(std::string(tag), std::string(c.text_trimmed()));
        }
        if (find_faction(f.name)) {
            diag(r, file, n.line(), "duplicate Faction " + f.name + " (first kept)");
            continue;
        }
        factions_.push_back(std::move(f));
        ++r.factions;
    }
}

bool GameAudioData::load(const FileProvider& files, GameAudioLoadReport* report) {
    GameAudioLoadReport local;
    GameAudioLoadReport& r = report ? *report : local;
    bool any = false;
    std::string text;
    if (read_text(files, "Data\\XML\\WeatherAudio.xml", text)) {
        parse_weather_audio_xml(text, "WeatherAudio.xml", r);
        any = true;
    } else {
        diag(r, "WeatherAudio.xml", 0, "missing");
    }
    if (read_text(files, "Data\\XML\\AmbientMapSounds.xml", text)) {
        parse_ambient_map_sounds_xml(text, "AmbientMapSounds.xml", r);
        any = true;
    } else {
        diag(r, "AmbientMapSounds.xml", 0, "missing");
    }
    if (read_text(files, "Data\\XML\\FactionFiles.xml", text)) {
        XmlDocument list = XmlDocument::parse(text, "FactionFiles.xml");
        for (XmlNode f : list.root().children("File")) {
            const std::string name(data::trim(f.text_trimmed()));
            std::string body;
            if (!read_text(files, "Data\\XML\\" + name, body)) {
                diag(r, "FactionFiles.xml", f.line(), "listed file missing: " + name);
                continue;
            }
            parse_factions_xml(body, name, r);
            any = true;
        }
    } else {
        diag(r, "FactionFiles.xml", 0, "missing");
    }
    if (read_text(files, "Data\\XML\\AnimationSFXMaps.txt", text)) {
        const size_t before = anim_.entry_count();
        anim_.parse(text, "AnimationSFXMaps.txt", &r.diagnostics);
        r.anim_sfx_entries += static_cast<uint32_t>(anim_.entry_count() - before);
        any = true;
    } else {
        diag(r, "AnimationSFXMaps.txt", 0, "missing");
    }
    return any;
}

}  // namespace alamo::audio
