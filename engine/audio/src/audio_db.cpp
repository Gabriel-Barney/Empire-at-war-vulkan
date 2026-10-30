// Audio XML databases (see alamo/audio/audio_db.hpp).
#include "alamo/audio/audio_db.hpp"

#include <cstdio>
#include <functional>
#include <optional>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::audio {
namespace {

using data::XmlDocument;
using data::XmlNode;

// atoi-style leading integer ("80, 20" -> 80, " -1 " -> -1). The original's byte fields take the
// first number of such values.
std::optional<int32_t> leading_int(std::string_view s) {
    s = data::trim(s);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    int64_t v = 0;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFFll) return std::nullopt;
        ++i;
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    return static_cast<int32_t>(neg ? -v : v);
}

std::vector<std::string> to_strings(std::string_view s) {
    std::vector<std::string> out;
    for (std::string_view t : data::split_list(s)) out.emplace_back(t);
    return out;
}

// Removes surrounding double quotes (MSS_3D_Provider_Name values are quoted).
std::string unquote(std::string_view s) {
    s = data::trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return std::string(s);
}

struct Ctx {
    AudioLoadReport* report;
    std::string_view file;
    uint32_t line;
    std::string_view tag;
    void bad(std::string_view value) const {
        ++report->bad_values;
        char buf[64];
        std::snprintf(buf, sizeof buf, "(%u): bad value for ", line);
        report->diagnostics.push_back(std::string(file) + buf + std::string(tag) + ": '" + std::string(value) + "'");
    }
};

void set_bool(const Ctx& c, std::string_view v, bool& out) {
    if (auto b = data::parse_bool(v)) out = *b;
    else c.bad(v);
}
void set_int(const Ctx& c, std::string_view v, int32_t& out) {
    if (auto i = leading_int(v)) out = *i;
    else c.bad(v);
}
void set_float(const Ctx& c, std::string_view v, float& out) {
    if (auto f = data::parse_float(v)) out = *f;
    else c.bad(v);
}

template <class T>
using Setter = std::function<void(const Ctx&, std::string_view, T&)>;
template <class T>
using FieldTable = std::map<std::string, Setter<T>>;  // key: normalize_name(tag)

#define ALAMO_B(tag, m) {normalize_name(tag), [](const Ctx& c, std::string_view v, auto& e) { set_bool(c, v, e.m); }}
#define ALAMO_I(tag, m) {normalize_name(tag), [](const Ctx& c, std::string_view v, auto& e) { set_int(c, v, e.m); }}
#define ALAMO_F(tag, m) {normalize_name(tag), [](const Ctx& c, std::string_view v, auto& e) { set_float(c, v, e.m); }}
#define ALAMO_S(tag, m) {normalize_name(tag), [](const Ctx&, std::string_view v, auto& e) { e.m = std::string(data::trim(v)); }}
#define ALAMO_L(tag, m) {normalize_name(tag), [](const Ctx&, std::string_view v, auto& e) { e.m = to_strings(v); }}

// xml_field_tables.md, segment `SFXEvent` (35 fields). Use_Preset is handled before the others.
const FieldTable<SfxEvent>& sfx_fields() {
    static const FieldTable<SfxEvent> t = {
        ALAMO_B("Is_Preset", is_preset),
        ALAMO_S("Use_Preset", use_preset),
        ALAMO_L("Samples", samples),
        ALAMO_L("Pre_Samples", pre_samples),
        ALAMO_L("Post_Samples", post_samples),
        ALAMO_L("Text_ID", text_ids),
        ALAMO_B("Play_Sequentially", play_sequentially),
        ALAMO_I("Priority", priority),
        ALAMO_I("Probability", probability),
        ALAMO_I("Play_Count", play_count),
        ALAMO_F("Loop_Fade_In_Seconds", loop_fade_in_seconds),
        ALAMO_F("Loop_Fade_Out_Seconds", loop_fade_out_seconds),
        ALAMO_I("Max_Instances", max_instances),
        ALAMO_I("Min_Volume", min_volume),
        ALAMO_I("Max_Volume", max_volume),
        ALAMO_I("Min_Pitch", min_pitch),
        ALAMO_I("Max_Pitch", max_pitch),
        ALAMO_I("Min_Pan2D", min_pan2d),
        ALAMO_I("Max_Pan2D", max_pan2d),
        ALAMO_I("Min_Predelay", min_predelay),
        ALAMO_I("Max_Predelay", max_predelay),
        ALAMO_I("Min_Postdelay", min_postdelay),
        ALAMO_I("Max_Postdelay", max_postdelay),
        ALAMO_F("Volume_Saturation_Distance", volume_saturation_distance),
        ALAMO_B("Kills_Previous_Object_SFX", kills_previous_object_sfx),
        ALAMO_S("Overlap_Test", overlap_test),
        ALAMO_B("Localize", localize),
        ALAMO_B("Is_2D", is_2d),
        ALAMO_B("Is_3D", is_3d),
        ALAMO_B("Is_GUI", is_gui),
        ALAMO_B("Is_HUD_VO", is_hud_vo),
        ALAMO_B("Is_Unit_Response_VO", is_unit_response_vo),
        ALAMO_B("Is_Ambient_VO", is_ambient_vo),
        ALAMO_S("Chained_SFXEvent", chained_sfx_event),
        ALAMO_S("Render_Mode", render_mode),
    };
    return t;
}

// Segment `MusicEvent` (7 fields).
const FieldTable<MusicEvent>& music_fields() {
    static const FieldTable<MusicEvent> t = {
        ALAMO_F("Fade_In_Seconds", fade_in_seconds),
        ALAMO_F("Fade_Out_Previous_Seconds", fade_out_previous_seconds),
        ALAMO_B("Loop", loop),
        ALAMO_B("MSS_Internal_Loop", mss_internal_loop),
        ALAMO_L("Files", files),
        ALAMO_L("Text_ID", text_ids),
        ALAMO_I("Volume_Percent", volume_percent),
    };
    return t;
}

// SpeechEvent has no table in xml_field_tables.md; these are the tags the shipped files use.
const FieldTable<SpeechEvent>& speech_fields() {
    static const FieldTable<SpeechEvent> t = {
        ALAMO_L("Files", files),
        ALAMO_L("Text_ID", text_ids),
        ALAMO_I("Volume_Percent", volume_percent),
        ALAMO_S("Overlap_Test", overlap_test),
    };
    return t;
}

// Segment `Audio` (35 fields).
const FieldTable<AudioConstants>& audio_fields() {
    static const FieldTable<AudioConstants> t = {
        ALAMO_F("Audio_Galactic_3D_Saturation_Distance_Mod", saturation_distance_mod[0]),
        ALAMO_F("Audio_Space_3D_Saturation_Distance_Mod", saturation_distance_mod[1]),
        ALAMO_F("Audio_Land_3D_Saturation_Distance_Mod", saturation_distance_mod[2]),
        ALAMO_F("Audio_Galactic_3D_Rolloff_Distance_Mod", rolloff_distance_mod[0]),
        ALAMO_F("Audio_Space_3D_Rolloff_Distance_Mod", rolloff_distance_mod[1]),
        ALAMO_F("Audio_Land_3D_Rolloff_Distance_Mod", rolloff_distance_mod[2]),
        ALAMO_F("Audio_Galactic_3D_Listener_Z_Pullback_Dist", listener_z_pullback[0]),
        ALAMO_F("Audio_Space_3D_Listener_Z_Pullback_Dist", listener_z_pullback[1]),
        ALAMO_F("Audio_Land_3D_Listener_Z_Pullback_Dist", listener_z_pullback[2]),
        ALAMO_S("SFXEvent_Planet_Capture_Bonus_Reward", sfx_planet_capture_bonus_reward),
        ALAMO_S("SFXEvent_GUI_Negative_Feedback", sfx_gui_negative_feedback),
        ALAMO_S("SFXEvent_Planet_Revealed", sfx_planet_revealed),
        ALAMO_S("SFXEvent_MP_Give_Teammate_Credits", sfx_mp_give_teammate_credits),
        ALAMO_F("Music_Land_Battle_To_Ambient_Peace_Seconds", music_land_battle_to_ambient_peace_seconds),
        ALAMO_F("Music_Space_Battle_To_Ambient_Peace_Seconds", music_space_battle_to_ambient_peace_seconds),
        ALAMO_S("Telekinesis_SFXEvent_Loop", telekinesis_sfx_loop),
        ALAMO_S("Telekinesis_SFXEvent_Damage", telekinesis_sfx_damage),
        ALAMO_S("Telekinesis_SFXEvent_Slam", telekinesis_sfx_slam),
        {normalize_name("MSS_3D_Provider_Name"),
         [](const Ctx&, std::string_view v, AudioConstants& e) { e.mss_3d_provider_names.push_back(unquote(v)); }},
        ALAMO_S("SFXEvent_Command_Bar_Attack", sfx_command_bar_attack),
        ALAMO_S("SFXEvent_Command_Bar_Attack_Move", sfx_command_bar_attack_move),
        ALAMO_S("SFXEvent_Command_Bar_Move", sfx_command_bar_move),
        ALAMO_S("SFXEvent_Command_Bar_Waypoint", sfx_command_bar_waypoint),
        ALAMO_S("SFXEvent_Command_Bar_Stop", sfx_command_bar_stop),
        ALAMO_S("SFXEvent_Command_Bar_Guard", sfx_command_bar_guard),
        ALAMO_I("Delay_Between_Space_Base_Attack_Announcement_Seconds", delay_between_space_base_attack_announcement_seconds),
        ALAMO_I("Delay_Between_Land_Base_Attack_Announcement_Seconds", delay_between_land_base_attack_announcement_seconds),
        ALAMO_S("Music_Event_Battle_Load_Screen", music_event_battle_load_screen),
        ALAMO_S("Music_Event_Battle_End_Summary_Screen_Win", music_event_battle_end_summary_screen_win),
        ALAMO_S("Music_Event_Battle_End_Summary_Screen_Lose", music_event_battle_end_summary_screen_lose),
        ALAMO_I("Default_MSS_DIG_DS_FRAGMENT_CNT", default_mss_dig_ds_fragment_cnt),
        ALAMO_I("Default_MSS_DIG_DS_MIX_FRAGMENT_CNT", default_mss_dig_ds_mix_fragment_cnt),
        ALAMO_I("Sustained_IO_MSS_DIG_DS_FRAGMENT_CNT", sustained_io_mss_dig_ds_fragment_cnt),
        ALAMO_I("Sustained_IO_MSS_DIG_DS_MIX_FRAGMENT_CNT", sustained_io_mss_dig_ds_mix_fragment_cnt),
        ALAMO_B("Test_Disable_Weapon_Fire_SFX", test_disable_weapon_fire_sfx),
    };
    return t;
}

#undef ALAMO_B
#undef ALAMO_I
#undef ALAMO_F
#undef ALAMO_S
#undef ALAMO_L

void note_unknown(AudioLoadReport& r, std::string_view type, std::string_view tag) {
    ++r.unknown_tags[std::string(type) + "." + std::string(tag)];
}

void diag(AudioLoadReport& r, std::string_view file, uint32_t line, const std::string& msg) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "(%u): ", line);
    r.diagnostics.push_back(std::string(file) + buf + msg);
}

template <class T>
void apply_one(XmlNode c, const FieldTable<T>& table, std::string_view type, std::string_view file,
               AudioLoadReport& r, T& e) {
    auto it = table.find(normalize_name(c.name()));
    if (it == table.end()) {
        note_unknown(r, type, c.name());
        return;
    }
    Ctx ctx{&r, file, c.line(), c.name()};
    it->second(ctx, c.text_trimmed(), e);
}

template <class T>
void apply_children(XmlNode n, const FieldTable<T>& table, std::string_view type, std::string_view file,
                    AudioLoadReport& r, T& e) {
    for (XmlNode c : n.children()) apply_one(c, table, type, file, r, e);
}

// Shared shape of the three event files: <Root> <Type Name="..."> fields </Type> ... </Root>.
template <class T>
void parse_events(std::string_view xml, std::string_view file, std::string_view type, AudioLoadReport& r,
                  std::vector<T>& out, std::map<std::string, uint32_t>& index, uint32_t& count, uint32_t& dups,
                  const std::function<void(XmlNode, T&)>& fill) {
    XmlDocument doc = XmlDocument::parse(xml, std::string(file));
    if (!doc.ok()) {
        diag(r, file, 0, "not parsable");
        return;
    }
    for (XmlNode c : doc.root().children()) {
        if (!iequals(c.name(), type)) {
            note_unknown(r, std::string(type) + "File", c.name());
            continue;
        }
        T e;
        e.name = std::string(data::trim(c.attribute("Name")));
        e.file = std::string(file);
        e.line = c.line();
        fill(c, e);
        const std::string key = normalize_name(e.name);
        if (index.count(key) != 0) {  // AUDIO.md §1.1: the first definition wins
            ++dups;
            diag(r, file, e.line, "duplicate " + std::string(type) + " '" + e.name + "' ignored");
            continue;
        }
        index.emplace(key, static_cast<uint32_t>(out.size()));
        out.push_back(std::move(e));
        ++count;
    }
}

uint32_t lookup(const std::map<std::string, uint32_t>& index, std::string_view name) {
    auto it = index.find(normalize_name(data::trim(name)));
    return it == index.end() ? kNoEvent : it->second;
}

bool read_text(const FileProvider& files, const std::string& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

}  // namespace

void AudioDatabase::parse_sfx_xml(std::string_view xml, std::string_view file, AudioLoadReport& r) {
    const auto& table = sfx_fields();
    const std::string use_preset_key = normalize_name("Use_Preset");
    parse_events<SfxEvent>(xml, file, "SFXEvent", r, sfx_, sfx_index_, r.sfx_events, r.sfx_duplicates,
                           [&](XmlNode n, SfxEvent& e) {
                               // AUDIO.md §1.1: Use_Preset copies every field of the preset at that point
                               // of parsing (earlier tags are overwritten), later tags override.
                               for (XmlNode c : n.children()) {
                                   if (normalize_name(c.name()) != use_preset_key) {
                                       apply_one(c, table, "SFXEvent", file, r, e);
                                       continue;
                                   }
                                   const std::string pname(c.text_trimmed());
                                   const uint32_t pi = lookup(sfx_index_, pname);
                                   if (pi == kNoEvent) {
                                       ++r.unresolved_presets;
                                       diag(r, file, c.line(), "unknown preset '" + pname + "'");
                                   } else {
                                       std::string name = e.name, f = e.file;
                                       const uint32_t line = e.line;
                                       e = sfx_[pi];
                                       e.name = std::move(name);
                                       e.file = std::move(f);
                                       e.line = line;
                                       e.is_preset = false;  // never a preset itself
                                       e.preset_index = pi;
                                   }
                                   e.use_preset = pname;
                               }
                           });
    r.sfx_presets = 0;
    for (const SfxEvent& e : sfx_) r.sfx_presets += e.is_preset ? 1u : 0u;
}

void AudioDatabase::parse_music_xml(std::string_view xml, std::string_view file, AudioLoadReport& r) {
    parse_events<MusicEvent>(xml, file, "MusicEvent", r, music_, music_index_, r.music_events, r.music_duplicates,
                             [&](XmlNode n, MusicEvent& e) { apply_children(n, music_fields(), "MusicEvent", file, r, e); });
}

void AudioDatabase::parse_speech_xml(std::string_view xml, std::string_view file, AudioLoadReport& r) {
    parse_events<SpeechEvent>(xml, file, "SpeechEvent", r, speech_, speech_index_, r.speech_events,
                              r.speech_duplicates, [&](XmlNode n, SpeechEvent& e) {
                                  apply_children(n, speech_fields(), "SpeechEvent", file, r, e);
                              });
}

void AudioDatabase::parse_audio_xml(std::string_view xml, std::string_view file, AudioLoadReport& r) {
    XmlDocument doc = XmlDocument::parse(xml, std::string(file));
    if (!doc.ok()) {
        diag(r, file, 0, "not parsable");
        return;
    }
    const std::string ambient = normalize_name("Ambient_Map_Sounds");
    for (XmlNode c : doc.root().children()) {
        const std::string key = normalize_name(c.name());
        if (key == ambient) {
            // Not in the Audio field table: kept for the ambient system, reported as unknown.
            note_unknown(r, "Audio", c.name());
            auto pair = data::parse_pair(c.text_trimmed());
            if (pair) constants_.ambient_map_sounds.emplace_back(std::string(pair->first), std::string(pair->second));
            continue;
        }
        auto it = audio_fields().find(key);
        if (it == audio_fields().end()) {
            note_unknown(r, "Audio", c.name());
            continue;
        }
        Ctx ctx{&r, file, c.line(), c.name()};
        it->second(ctx, c.text_trimmed(), constants_);
    }
}

void AudioDatabase::resolve_links(AudioLoadReport& r) {
    for (SfxEvent& e : sfx_) {
        e.chained_index = kNoEvent;
        if (e.chained_sfx_event.empty()) continue;
        e.chained_index = lookup(sfx_index_, e.chained_sfx_event);
        if (e.chained_index == kNoEvent) {
            ++r.unresolved_chains;
            diag(r, e.file, e.line, "unknown Chained_SFXEvent '" + e.chained_sfx_event + "'");
        }
    }
}

bool AudioDatabase::load(const FileProvider& files, AudioLoadReport* report) {
    AudioLoadReport local;
    AudioLoadReport& r = report ? *report : local;
    std::string text;
    // MAIN_LOOP.md startup: TheAudioConstants before the XML databases.
    if (read_text(files, "Data\\XML\\Audio.xml", text)) parse_audio_xml(text, "Audio.xml", r);
    else diag(r, "Audio.xml", 0, "missing");

    if (!read_text(files, "Data\\XML\\SFXEventFiles.xml", text)) {
        diag(r, "SFXEventFiles.xml", 0, "missing");
        return false;
    }
    XmlDocument list = XmlDocument::parse(text, "SFXEventFiles.xml");
    if (!list.ok()) {
        diag(r, "SFXEventFiles.xml", 0, "not parsable");
        return false;
    }
    for (XmlNode f : list.root().children("File")) {
        const std::string name(f.text_trimmed());
        if (name.empty()) continue;
        ++r.sfx_files;
        std::string body;
        if (!read_text(files, "Data\\XML\\" + name, body)) {
            ++r.sfx_files_missing;
            diag(r, name, 0, "listed in SFXEventFiles.xml but missing");
            continue;
        }
        parse_sfx_xml(body, name, r);
    }
    resolve_links(r);

    if (read_text(files, "Data\\XML\\MusicEvents.xml", text)) parse_music_xml(text, "MusicEvents.xml", r);
    else diag(r, "MusicEvents.xml", 0, "missing");
    if (read_text(files, "Data\\XML\\SpeechEvents.xml", text)) parse_speech_xml(text, "SpeechEvents.xml", r);
    else diag(r, "SpeechEvents.xml", 0, "missing");
    return true;
}

uint32_t AudioDatabase::find_sfx(std::string_view name) const { return lookup(sfx_index_, name); }
uint32_t AudioDatabase::find_music(std::string_view name) const { return lookup(music_index_, name); }
uint32_t AudioDatabase::find_speech(std::string_view name) const { return lookup(speech_index_, name); }

std::string AudioLoadReport::to_string() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "sfx files %u (missing %u), sfx events %u (presets %u, duplicates %u, unresolved presets %u, "
                  "unresolved chains %u)\nmusic events %u (duplicates %u), speech events %u (duplicates %u), bad "
                  "values %u\n",
                  sfx_files, sfx_files_missing, sfx_events, sfx_presets, sfx_duplicates, unresolved_presets,
                  unresolved_chains, music_events, music_duplicates, speech_events, speech_duplicates, bad_values);
    std::string s = buf;
    s += "unknown tags:";
    if (unknown_tags.empty()) s += " none";
    for (const auto& [k, n] : unknown_tags) s += " " + k + "=" + std::to_string(n);
    s += "\n";
    return s;
}

}  // namespace alamo::audio
