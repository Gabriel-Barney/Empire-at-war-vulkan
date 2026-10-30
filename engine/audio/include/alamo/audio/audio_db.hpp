// Audio databases: SFX events, music events, speech events and the Audio constants, parsed from the
// game XML (docs/re/xml_field_tables.md "SFXEvent", "MusicEvent", "Audio"; MAIN_LOOP.md startup
// order: TheAudioConstants, then SFXEventFiles, ..., MusicEvents, SpeechEvents).
//
// Files (through any FileProvider, normally the VFS):
//   Data\XML\Audio.xml              -> AudioConstants
//   Data\XML\SFXEventFiles.xml      -> <File> list, parsed in order ("Presets must go first")
//   Data\XML\MusicEvents.xml        -> MusicEvent
//   Data\XML\SpeechEvents.xml       -> SpeechEvent
//
// Field semantics and defaults follow docs/re/AUDIO.md §1.1, §2.1, §3.1:
//   * Use_Preset copies every field of the named (earlier) event at that point of parsing; tags before
//     it are overwritten, later tags override. The event keeps its own name and is never a preset.
//   * Duplicate event names: the first definition wins; later ones are counted in the report.
//   * Byte-sized fields (Priority, Probability, Play_Count, Max_Instances, volumes, pitches) keep the
//     XML value as int32 [port]; every shipped value fits the original's byte storage.
//   * Defaults of fields an event never sets: SfxEvent member initialisers (§1.1 table).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo::audio {

inline constexpr uint32_t kNoEvent = 0xFFFFFFFFu;

struct SfxEvent {
    std::string name;
    std::string file;  // XML file it came from (diagnostics)
    uint32_t line = 0;

    bool is_preset = false;
    std::string use_preset;          // as written
    std::vector<std::string> samples;
    std::vector<std::string> pre_samples;
    std::vector<std::string> post_samples;
    std::vector<std::string> text_ids;
    bool play_sequentially = false;
    int32_t priority = 3;            // 1 = most important
    int32_t probability = 100;       // percent
    int32_t play_count = 1;          // -1 = loop until stopped
    float loop_fade_in_seconds = 0.0f;
    float loop_fade_out_seconds = 0.0f;
    int32_t max_instances = 1;       // 0 = never plays
    int32_t min_volume = 100, max_volume = 100;    // percent
    int32_t min_pitch = 100, max_pitch = 100;      // percent of the original rate
    int32_t min_pan2d = 50, max_pan2d = 50;        // 0..100, 50 = centre (2D only)
    int32_t min_predelay = 0, max_predelay = 0;    // milliseconds
    int32_t min_postdelay = 0, max_postdelay = 0;  // milliseconds
    float volume_saturation_distance = 300.0f;     // 3D min distance
    bool kills_previous_object_sfx = false;
    std::string overlap_test;
    bool localize = false;
    bool is_2d = false;
    bool is_3d = true;  // default on; wins over Is_2D
    bool is_gui = false;
    bool is_hud_vo = false;
    bool is_unit_response_vo = false;
    bool is_ambient_vo = false;
    std::string chained_sfx_event;
    std::string render_mode;

    uint32_t preset_index = kNoEvent;   // resolved Use_Preset
    uint32_t chained_index = kNoEvent;  // resolved Chained_SFXEvent (after all files are loaded)
};

struct MusicEvent {
    std::string name;
    std::string file;
    uint32_t line = 0;
    std::vector<std::string> files;  // Data\Audio\Music\<file>
    std::vector<std::string> text_ids;
    int32_t volume_percent = 100;
    float fade_in_seconds = 0.0f;
    float fade_out_previous_seconds = 0.0f;
    bool loop = false;
    bool mss_internal_loop = false;
};

struct SpeechEvent {
    std::string name;
    std::string file;
    uint32_t line = 0;
    std::vector<std::string> files;  // Data\Audio\Speech\<Language>\<file>
    std::vector<std::string> text_ids;
    int32_t volume_percent = 100;
    std::string overlap_test;  // EaW SpeechEvents.xml only
};

// Audio.xml (xml_field_tables.md segment `Audio`, 35 fields). Index 0/1/2 = galactic/space/land.
enum class AudioMode : uint8_t { Galactic = 0, Space = 1, Land = 2 };

struct AudioConstants {
    float saturation_distance_mod[3] = {1.0f, 1.0f, 1.0f};
    float rolloff_distance_mod[3] = {1.0f, 1.0f, 1.0f};
    float listener_z_pullback[3] = {0.0f, 0.0f, 0.0f};
    std::string sfx_planet_capture_bonus_reward;
    std::string sfx_gui_negative_feedback;
    std::string sfx_planet_revealed;
    std::string sfx_mp_give_teammate_credits;
    float music_land_battle_to_ambient_peace_seconds = 0.0f;
    float music_space_battle_to_ambient_peace_seconds = 0.0f;
    std::string telekinesis_sfx_loop, telekinesis_sfx_damage, telekinesis_sfx_slam;
    std::vector<std::string> mss_3d_provider_names;
    std::string sfx_command_bar_attack, sfx_command_bar_attack_move, sfx_command_bar_move,
        sfx_command_bar_waypoint, sfx_command_bar_stop, sfx_command_bar_guard;
    int32_t delay_between_space_base_attack_announcement_seconds = 0;
    int32_t delay_between_land_base_attack_announcement_seconds = 0;
    std::string music_event_battle_load_screen;
    std::string music_event_battle_end_summary_screen_win;
    std::string music_event_battle_end_summary_screen_lose;
    int32_t default_mss_dig_ds_fragment_cnt = 0, default_mss_dig_ds_mix_fragment_cnt = 0;
    int32_t sustained_io_mss_dig_ds_fragment_cnt = 0, sustained_io_mss_dig_ds_mix_fragment_cnt = 0;
    bool test_disable_weapon_fire_sfx = false;
    // Not in the Audio field table (parsed by another class or dead); kept as written.
    std::vector<std::pair<std::string, std::string>> ambient_map_sounds;
};

struct AudioLoadReport {
    uint32_t sfx_files = 0, sfx_files_missing = 0;
    uint32_t sfx_events = 0, sfx_presets = 0, sfx_duplicates = 0;
    uint32_t music_events = 0, music_duplicates = 0;
    uint32_t speech_events = 0, speech_duplicates = 0;
    uint32_t unresolved_presets = 0, unresolved_chains = 0;
    uint32_t bad_values = 0;  // a known tag whose value did not parse (field left unchanged)
    // "<Type>.<Tag>" -> count for tags not in the field tables (kept out of the databases).
    std::map<std::string, uint32_t> unknown_tags;
    std::vector<std::string> diagnostics;  // "file(line): message"
    std::string to_string() const;
};

class AudioDatabase {
public:
    // Loads everything above. Missing files are diagnostics, not failures; returns false only when
    // SFXEventFiles.xml is missing or unparsable.
    bool load(const FileProvider& files, AudioLoadReport* report = nullptr);

    // Parse single documents (tests; also used by load). Events are appended in order.
    void parse_sfx_xml(std::string_view xml, std::string_view file, AudioLoadReport& report);
    void parse_music_xml(std::string_view xml, std::string_view file, AudioLoadReport& report);
    void parse_speech_xml(std::string_view xml, std::string_view file, AudioLoadReport& report);
    void parse_audio_xml(std::string_view xml, std::string_view file, AudioLoadReport& report);
    // Resolves Chained_SFXEvent names (call after the last SFX file).
    void resolve_links(AudioLoadReport& report);

    const std::vector<SfxEvent>& sfx() const { return sfx_; }
    const std::vector<MusicEvent>& music() const { return music_; }
    const std::vector<SpeechEvent>& speech() const { return speech_; }
    const AudioConstants& constants() const { return constants_; }

    // Case-insensitive name lookups; kNoEvent if absent.
    uint32_t find_sfx(std::string_view name) const;
    uint32_t find_music(std::string_view name) const;
    uint32_t find_speech(std::string_view name) const;

private:
    std::vector<SfxEvent> sfx_;
    std::vector<MusicEvent> music_;
    std::vector<SpeechEvent> speech_;
    AudioConstants constants_;
    std::map<std::string, uint32_t> sfx_index_, music_index_, speech_index_;  // normalize_name keys
};

}  // namespace alamo::audio
