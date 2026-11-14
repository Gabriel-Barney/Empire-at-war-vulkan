// Game-side audio tables the event player itself does not need: weather / map ambience and the per-faction
// music, announcement and speech events. Read by GameAudio (game_audio.hpp) and AmbienceManager
// (ambience.hpp).
//
// Files (through any FileProvider, normally the VFS):
//   Data\XML\WeatherAudio.xml       -> WeatherAudioTable (xml_field_tables.md segment `WeatherAudio`)
//   Data\XML\AmbientMapSounds.xml   -> AmbientMapSound list (referenced from Audio.xml Ambient_Map_Sounds)
//   Data\XML\FactionFiles.xml       -> listed files (Factions.xml, Expansion_Factions.xml): FactionAudio
//   Data\XML\AnimationSFXMaps.txt   -> AnimationSfxMap
//
// Value formats (as shipped; AUDIO.md §6.2 "WeatherAudioManager: intensity-selected loops with fade
// in/out and intermittent events with min/max seconds"):
//   <Weather_SFXEvent_Loop>         TYPE, intensity, event                       (event "None" = silence)
//   <Weather_SFXEvent_Intermittent> TYPE, intensity, event, min_s, max_s
//   <Ambient_SFXEvent_Intermittent> ENVIRONMENT, event, min_s, max_s             ("when weather is clear")
//   <AmbientMapSound Name=..><Ambient> event, closest, farthest, min_s, max_s    (file comment)
// Intensity selection [inferred from the data, which lists each type's rows from heavy to none]: the row
// whose intensity is the largest one <= the current intensity; no row -> nothing.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/audio/audio_db.hpp"
#include "alamo/core/file_provider.hpp"

namespace alamo::audio {

struct WeatherLoopEntry {
    std::string weather;    // CLEAR, RAIN, SNOW, SANDSTORM, ASH (as written)
    float intensity = 0.0f; // lower bound of the intensity range
    std::string event;      // "" when the data says None
};

struct WeatherIntermittentEntry {
    std::string weather;
    float intensity = 0.0f;
    std::string event;  // "" = None
    float min_seconds = 0.0f, max_seconds = 0.0f;
};

struct AmbientIntermittentEntry {
    std::string environment;  // TEMPERATE, ARCTIC, DESERT, FOREST, SWAMP, VOLCANIC, URBAN
    std::string event;
    float min_seconds = 0.0f, max_seconds = 0.0f;
};

struct WeatherAudioTable {
    std::vector<WeatherLoopEntry> loops;
    std::vector<WeatherIntermittentEntry> intermittents;
    std::vector<AmbientIntermittentEntry> ambient;

    // Intensity selection (see above). Weather type match is case-insensitive.
    const WeatherLoopEntry* loop_for(std::string_view weather, float intensity) const;
    const WeatherIntermittentEntry* intermittent_for(std::string_view weather, float intensity) const;
    const AmbientIntermittentEntry* ambient_for(std::string_view environment) const;  // first row
};

struct AmbientMapSoundEntry {
    std::string event;
    float min_distance = 0.0f, max_distance = 0.0f;  // "closest / farthest surround dist"
    float min_seconds = 0.0f, max_seconds = 0.0f;
};
struct AmbientMapSound {
    std::string name;
    std::vector<AmbientMapSoundEntry> entries;
};

// Audio-related tags of one <Faction> element (every child whose tag contains "SFX", "Music" or
// "Speech", plus Faction_Super_Weapon), kept raw in document order so the game host and GameAudio can read any of the ~70 tags.
struct FactionAudio {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;  // tag as written, trimmed text

    // Last occurrence of `tag` (case-insensitive), "" if absent or empty.
    std::string_view get(std::string_view tag) const;
    // "KEY, value" rows of a repeated tag (Music_Event_List_Ambient, *_Vs_Faction, SFXEvent_Weather_Begin,
    // SFXEvent_Unit_Type_Spotted, ...): every value whose key matches (case-insensitive), in order.
    std::vector<std::string> pair_values(std::string_view tag, std::string_view key) const;
};

// Data\XML\AnimationSFXMaps.txt (AUDIO.md §5, §6.2 "animation events"): tab/space separated lines
//   SOUND    <animation> <time ms> <SFX event>
//   SURFACE  <animation> <time ms> <surface fx> <bone>     (surface-dependent effect, for the particle host)
// Entries of one animation are kept sorted by time (stable for equal times). Times are milliseconds from
// the start of the animation [H format, M timing].
struct AnimSfxEntry {
    enum class Kind : uint8_t { Sound, Surface };
    Kind kind = Kind::Sound;
    uint32_t time_ms = 0;
    std::string name;  // SFX event (Sound) or surface effect type (Surface)
    std::string bone;  // Surface only
};

class AnimationSfxMap {
public:
    void parse(std::string_view text, std::string_view file, std::vector<std::string>* diagnostics = nullptr);
    // Every entry of `animation` (case-insensitive) with prev_ms < time <= now_ms, in time order. Pass
    // prev_ms = -1 on the animation's first frame so time-0 entries fire; a looping animation that wrapped
    // is two calls (prev..length, -1..now).
    std::vector<const AnimSfxEntry*> crossed(std::string_view animation, int64_t prev_ms, int64_t now_ms) const;
    const std::vector<AnimSfxEntry>* entries(std::string_view animation) const;
    size_t animation_count() const { return by_anim_.size(); }
    size_t entry_count() const { return entries_; }

private:
    std::map<std::string, std::vector<AnimSfxEntry>> by_anim_;  // normalize_name(animation)
    size_t entries_ = 0;
};

struct GameAudioLoadReport {
    uint32_t weather_loops = 0, weather_intermittents = 0, ambient_intermittents = 0;
    uint32_t ambient_map_sounds = 0, factions = 0;
    uint32_t anim_sfx_entries = 0;
    std::vector<std::string> diagnostics;  // "file(line): message"
};

class GameAudioData {
public:
    // Missing files are diagnostics. Returns false only if nothing at all could be read.
    bool load(const FileProvider& files, GameAudioLoadReport* report = nullptr);

    void parse_weather_audio_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r);
    void parse_ambient_map_sounds_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r);
    void parse_factions_xml(std::string_view xml, std::string_view file, GameAudioLoadReport& r);

    const WeatherAudioTable& weather() const { return weather_; }
    const AnimationSfxMap& animation_sfx() const { return anim_; }
    AnimationSfxMap& animation_sfx() { return anim_; }
    const std::vector<AmbientMapSound>& ambient_map_sounds() const { return ambient_; }
    const std::vector<FactionAudio>& factions() const { return factions_; }
    const FactionAudio* find_faction(std::string_view name) const;          // first definition
    const AmbientMapSound* find_ambient_map_sound(std::string_view name) const;

private:
    WeatherAudioTable weather_;
    std::vector<AmbientMapSound> ambient_;
    std::vector<FactionAudio> factions_;
    AnimationSfxMap anim_;
};

}  // namespace alamo::audio
