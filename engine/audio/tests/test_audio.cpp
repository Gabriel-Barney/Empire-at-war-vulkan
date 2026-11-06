// alamo_audio tests (no framework; non-zero exit on failure). Rules: docs/re/AUDIO.md (§ refs below).
//   * XML parsing on synthetic files (defaults, in-order Use_Preset, unknown tags, odd values, chains);
//   * SFX gates in the original order, per-cycle draws checked draw by draw against a replica of the free
//     RNG, the instance state machine, chaining (2D only), stop fades, voice pools with priority/distance
//     stealing, 3D distance model, buses and speech ducking, the music playlist/crossfade/mode switches and
//     the speech FIFO, all on the null backend; the same session twice gives the same log;
//   * WAV decode + the miniaudio backend in no-device mode (a mix is pulled and checked);
//   * real data (ALAMO_FOC_DATA = FoC "corruption" folder, ALAMO_EAW_DATA = EaW "GameData" folder; skipped
//     when unset): every SFX/music/speech event parsed (counts, unknown fields), every referenced file
//     resolved, a sample of shipped files probed/decoded. Reports: logs/audio_<game>_report.txt.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "alamo/audio/audio_db.hpp"
#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/decode.hpp"
#ifdef ALAMO_AUDIO_HAVE_LUA
#include "alamo/audio/lua_audio.hpp"
#endif
#include "alamo/audio/miniaudio_backend.hpp"
#include "alamo/audio/sample_source.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/vfs/vfs.hpp"

using namespace alamo;
using namespace alamo::audio;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

// ------------------------------------------------------------------ fixtures

class MemoryProvider final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) {
        files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end());
    }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem:" + std::string(path) : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

const char* kPresets = R"(<?xml version="1.0"?>
<SFXEvents>
  <SFXEvent Name="Preset_UR">
    <Is_Preset> Yes </Is_Preset>
    <Is_2D> Yes </Is_2D>
    <Is_3D> No </Is_3D>
    <Is_Unit_Response_VO> Yes </Is_Unit_Response_VO>
    <Priority> 1 </Priority>
    <Max_Instances>1</Max_Instances>
    <Min_Volume>90</Min_Volume>
    <Max_Volume>90</Max_Volume>
  </SFXEvent>
  <SFXEvent Name="Preset_GUN">
    <Is_Preset> Yes </Is_Preset>
    <Is_3D> Yes </Is_3D>
    <Priority> 4 </Priority>
    <Max_Instances> 4 </Max_Instances>
    <Volume_Saturation_Distance>200.0</Volume_Saturation_Distance>
  </SFXEvent>
  <SFXEvent Name="Preset_LOOP">
    <Is_Preset> Yes </Is_Preset>
    <Is_3D> Yes </Is_3D>
    <Priority> 1 </Priority>
    <Max_Instances> 4 </Max_Instances>
    <Play_Count>-1</Play_Count>
    <Loop_Fade_In_Seconds> 0.5 </Loop_Fade_In_Seconds>
    <Loop_Fade_Out_Seconds> 1.0 </Loop_Fade_Out_Seconds>
  </SFXEvent>
</SFXEvents>
)";

const char* kEvents = R"(<?xml version="1.0"?>
<SFXEvents>
  <SFXEvent Name="Unit_Select_A">
    <Use_Preset>Preset_UR</Use_Preset>
    <Samples>A1_ENG.WAV A2_ENG.WAV A3_ENG.WAV</Samples>
    <Play_Sequentially> Yes </Play_Sequentially>
    <Localize> Yes </Localize>
    <Text_ID>TEXT_A1, TEXT_A2, TEXT_A3</Text_ID>
    <Overlap_Test> Unit_A </Overlap_Test>
  </SFXEvent>
  <SFXEvent Name="Unit_Move_A">
    <Min_Volume>50</Min_Volume>
    <Use_Preset>Preset_UR</Use_Preset>
    <Max_Volume>95</Max_Volume>
    <Samples>M1.WAV, M2.WAV</Samples>
    <Overlap_Test> Unit_A </Overlap_Test>
  </SFXEvent>
  <SFXEvent Name="Gun">
    <Use_Preset>Preset_GUN</Use_Preset>
    <Samples>G1.WAV G2.WAV G3.WAV G4.WAV</Samples>
    <Min_Volume>80</Min_Volume> <Max_Volume>90</Max_Volume>
    <Min_Pitch>95</Min_Pitch> <Max_Pitch>105</Max_Pitch>
    <Probability>80, 20</Probability>
    <Bogus_Tag>1</Bogus_Tag>
  </SFXEvent>
  <SFXEvent Name="Gun3D">
    <Use_Preset>Preset_GUN</Use_Preset>
    <Samples>G1.WAV</Samples>
  </SFXEvent>
  <SFXEvent Name="Laser">
    <Use_Preset>Preset_GUN</Use_Preset>
    <Samples>L1.WAV</Samples>
    <Max_Instances>2</Max_Instances>
  </SFXEvent>
  <SFXEvent Name="Chatter">
    <Is_3D>No</Is_3D>
    <Samples>C1.WAV</Samples>
    <Probability>60</Probability>
    <Min_Predelay>1000</Min_Predelay> <Max_Predelay>5000</Max_Predelay>
  </SFXEvent>
  <SFXEvent Name="Bomb_1">
    <Is_3D>No</Is_3D>
    <Samples>B1.WAV</Samples>
    <Pre_Samples>PRE.WAV</Pre_Samples>
    <Post_Samples>POST.WAV</Post_Samples>
    <Play_Count>2</Play_Count>
    <Chained_SFXEvent>Bomb_2</Chained_SFXEvent>
  </SFXEvent>
  <SFXEvent Name="Bomb_2">
    <Is_3D>No</Is_3D>
    <Samples>B2.WAV</Samples>
  </SFXEvent>
  <SFXEvent Name="Boom3D">
    <Samples>B1.WAV</Samples>
    <Chained_SFXEvent>Bomb_2</Chained_SFXEvent>
  </SFXEvent>
  <SFXEvent Name="VO_Seq">
    <Is_3D>No</Is_3D>
    <Max_Instances>3</Max_Instances>
    <Play_Sequentially>Yes</Play_Sequentially>
    <Pre_Samples>P1.WAV P2.WAV</Pre_Samples>
    <Samples>S1.WAV S2.WAV</Samples>
    <Post_Samples>Q1.WAV Q2.WAV</Post_Samples>
  </SFXEvent>
  <SFXEvent Name="Engine_Loop">
    <Use_Preset>Preset_LOOP</Use_Preset>
    <Samples>LOOP.WAV</Samples>
  </SFXEvent>
  <SFXEvent Name="Engine_Move_Loop">
    <Use_Preset>Preset_LOOP</Use_Preset>
    <Samples>LOOP.WAV</Samples>
    <Kills_Previous_Object_SFX>Yes</Kills_Previous_Object_SFX>
  </SFXEvent>
  <SFXEvent Name="Ambient_Low">
    <Is_3D>No</Is_3D>
    <Samples>AMB.WAV</Samples>
    <Priority>5</Priority>
    <Max_Instances>4</Max_Instances>
  </SFXEvent>
  <SFXEvent Name="Click">
    <Is_3D>No</Is_3D> <Is_GUI>Yes</Is_GUI>
    <Samples>CLICK.WAV</Samples>
    <Priority>1</Priority>
    <Max_Instances>4</Max_Instances>
  </SFXEvent>
  <SFXEvent Name="GUI_Bad_Sound">
    <Is_3D>No</Is_3D> <Is_GUI>Yes</Is_GUI>
    <Samples>BAD.WAV</Samples>
  </SFXEvent>
  <SFXEvent Name="HUD_Line">
    <Is_3D>No</Is_3D> <Is_HUD_VO>Yes</Is_HUD_VO>
    <Samples>HUD.WAV</Samples>
  </SFXEvent>
  <SFXEvent Name="Never">
    <Is_3D>No</Is_3D>
    <Samples>N.WAV</Samples>
    <Max_Instances>0</Max_Instances>
  </SFXEvent>
  <SFXEvent Name="Panned">
    <Is_3D>No</Is_3D>
    <Samples>PAN.WAV</Samples>
    <Min_Pan2D>0</Min_Pan2D> <Max_Pan2D>100</Max_Pan2D>
  </SFXEvent>
  <SFXEvent Name="Broken">
    <Samples>X.WAV</Samples>
    <Use_Preset>Preset_Missing</Use_Preset>
    <Chained_SFXEvent>Nope</Chained_SFXEvent>
    <Max_Volume>loud</Max_Volume>
  </SFXEvent>
  <SFXEvent Name="gun">
    <Samples>DUP.WAV</Samples>
  </SFXEvent>
</SFXEvents>
)";

const char* kMusic = R"(<?xml version="1.0"?>
<MusicEvents>
  <MusicEvent Name="Menu">
    <Files>MENU.MP3</Files>
    <Volume_Percent>80</Volume_Percent>
    <Fade_In_Seconds>0.0</Fade_In_Seconds>
    <Fade_Out_Previous_Seconds>2.0</Fade_Out_Previous_Seconds>
    <Loop>Yes</Loop>
  </MusicEvent>
  <MusicEvent Name="Galactic">
    <Files> G1.MP3, G2.MP3, G3.MP3 </Files>
    <Volume_Percent>60</Volume_Percent>
    <Fade_In_Seconds>2.0</Fade_In_Seconds>
    <Fade_Out_Previous_Seconds>1.0</Fade_Out_Previous_Seconds>
    <Loop> Yes </Loop>
    <Play_Sequentially>Yes</Play_Sequentially>
  </MusicEvent>
  <MusicEvent Name="Sting">
    <Files>STING.MP3</Files>
    <Volume_Percent>100</Volume_Percent>
    <Fade_In_Seconds>0.0</Fade_In_Seconds>
    <Fade_Out_Previous_Seconds>0.0</Fade_Out_Previous_Seconds>
    <Loop>No</Loop>
  </MusicEvent>
</MusicEvents>
)";

const char* kSpeech = R"(<?xml version="1.0"?>
<SpeechEvents>
  <SpeechEvent Name="Line_1"> <Files> L1_ENG.MP3 </Files> <Text_ID> T1 </Text_ID> <Volume_Percent> 90 </Volume_Percent> </SpeechEvent>
  <SpeechEvent Name="Line_2"> <Files> L2_ENG.MP3 </Files> <Text_ID> T2 </Text_ID> <Volume_Percent> 90 </Volume_Percent>
    <Overlap_Test> Tut </Overlap_Test> </SpeechEvent>
  <SpeechEvent Name="Line_3"> <Files> L3_ENG.MP3 </Files> <Volume_Percent> 50 </Volume_Percent>
    <Overlap_Test> Tut </Overlap_Test> </SpeechEvent>
</SpeechEvents>
)";

const char* kAudio = R"(<?xml version="1.0" ?>
<Audio>
  <Audio_Galactic_3D_Saturation_Distance_Mod> 1.0 </Audio_Galactic_3D_Saturation_Distance_Mod>
  <Audio_Space_3D_Saturation_Distance_Mod> 1.5 </Audio_Space_3D_Saturation_Distance_Mod>
  <Audio_Land_3D_Saturation_Distance_Mod> 1.0 </Audio_Land_3D_Saturation_Distance_Mod>
  <Audio_Galactic_3D_Rolloff_Distance_Mod> 1.0 </Audio_Galactic_3D_Rolloff_Distance_Mod>
  <Audio_Space_3D_Rolloff_Distance_Mod> 2.0 </Audio_Space_3D_Rolloff_Distance_Mod>
  <Audio_Land_3D_Rolloff_Distance_Mod> 10.0 </Audio_Land_3D_Rolloff_Distance_Mod>
  <Audio_Land_3D_Listener_Z_Pullback_Dist> 60.0 </Audio_Land_3D_Listener_Z_Pullback_Dist>
  <Ambient_Map_Sounds> Temperate, Ambient_Map_Sounds_Temperate </Ambient_Map_Sounds>
  <SFXEvent_GUI_Negative_Feedback> GUI_Bad_Sound </SFXEvent_GUI_Negative_Feedback>
  <MSS_3D_Provider_Name> "Miles Fast 2D Positional Audio" </MSS_3D_Provider_Name>
  <Music_Land_Battle_To_Ambient_Peace_Seconds> 15.0 </Music_Land_Battle_To_Ambient_Peace_Seconds>
  <Music_Space_Battle_To_Ambient_Peace_Seconds> 10.0 </Music_Space_Battle_To_Ambient_Peace_Seconds>
</Audio>
)";

MemoryProvider make_tree() {
    MemoryProvider p;
    p.add("Data\\XML\\SFXEventFiles.xml",
          "<SFXEvent_Files><File> SFXEventPresets.xml </File><File>SFXEventsTest.xml</File>"
          "<File>Missing.xml</File></SFXEvent_Files>");
    p.add("Data\\XML\\SFXEventPresets.xml", kPresets);
    p.add("Data\\XML\\SFXEventsTest.xml", kEvents);
    p.add("Data\\XML\\MusicEvents.xml", kMusic);
    p.add("Data\\XML\\SpeechEvents.xml", kSpeech);
    p.add("Data\\XML\\Audio.xml", kAudio);
    return p;
}

std::optional<double> test_duration(const SampleRef& r) {
    if (iequals(r.file, "MISSING.WAV")) return std::nullopt;
    if (iequals(r.file, "LOOP.WAV")) return 2.0;
    if (r.bus == Bus::Music) return 10.0;
    if (r.bus == Bus::Speech) return 3.0;
    return 1.0;
}

const LogEntry* last_op(const AudioSystem& a, LogOp op) {
    for (auto it = a.log().rbegin(); it != a.log().rend(); ++it)
        if (it->op == op) return &*it;
    return nullptr;
}

std::string last_reason(const AudioSystem& a) {
    const LogEntry* e = last_op(a, LogOp::SfxReject);
    return e ? e->reason : std::string();
}

int count_op(const AudioSystem& a, LogOp op, std::string_view event = {}) {
    int n = 0;
    for (const LogEntry& e : a.log()) n += (e.op == op && (event.empty() || iequals(e.event, event))) ? 1 : 0;
    return n;
}

std::vector<std::string> voice_samples(const AudioSystem& a) {
    std::vector<std::string> v;
    for (const LogEntry& e : a.log())
        if (e.op == LogOp::SfxVoice) v.push_back(e.sample);
    return v;
}

uint32_t voice_of(const AudioSystem& a, uint32_t handle) {
    uint32_t v = 0;
    for (const LogEntry& e : a.log())
        if (e.op == LogOp::SfxVoice && e.handle == handle) v = e.voice;
    return v;
}

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

SfxPlay at(float x, float y = 0.0f, float z = 0.0f) {
    SfxPlay p;
    p.kind = SfxPlay::Kind::Position;
    p.position = {x, y, z};
    return p;
}
SfxPlay on_object(uint64_t owner) {
    SfxPlay p;
    p.kind = SfxPlay::Kind::Object;
    p.owner = owner;
    return p;
}

constexpr float kSfxBus = 0.8f * 0.8f;  // default bus level x master (§4)

// ------------------------------------------------------------------ XML (§1.1)

void test_parse() {
    MemoryProvider tree = make_tree();
    AudioDatabase db;
    AudioLoadReport r;
    CHECK(db.load(tree, &r));
    CHECK(r.sfx_files == 3 && r.sfx_files_missing == 1);
    CHECK(r.sfx_presets == 3);
    CHECK(r.sfx_events == 22);  // 3 presets + 19 (the duplicate "gun" is not counted)
    CHECK(r.sfx_duplicates == 1);
    CHECK(r.unresolved_presets == 1 && r.unresolved_chains == 1);
    CHECK(r.bad_values == 1);
    CHECK(r.unknown_tags.count("SFXEvent.Bogus_Tag") == 1);
    CHECK(r.unknown_tags.count("MusicEvent.Play_Sequentially") == 1);  // EaW-only tag, not in FoC's table
    CHECK(r.unknown_tags.count("Audio.Ambient_Map_Sounds") == 1);
    CHECK(r.music_events == 3 && r.speech_events == 3);

    const SfxEvent& sel = db.sfx()[db.find_sfx("unit_select_a")];
    CHECK(sel.samples.size() == 3 && sel.samples[2] == "A3_ENG.WAV");
    CHECK(sel.text_ids.size() == 3 && sel.text_ids[1] == "TEXT_A2");
    CHECK(sel.is_2d && !sel.is_3d && sel.is_unit_response_vo && !sel.is_preset);
    CHECK(sel.priority == 1 && sel.max_instances == 1 && sel.min_volume == 90);
    CHECK(sel.overlap_test == "Unit_A" && sel.play_sequentially && sel.localize);
    CHECK(sel.preset_index == db.find_sfx("Preset_UR"));
    // Use_Preset copies at its position: a tag before it is overwritten, a tag after it wins.
    const SfxEvent& mv = db.sfx()[db.find_sfx("Unit_Move_A")];
    CHECK(mv.min_volume == 90 && mv.max_volume == 95);
    CHECK(mv.samples.size() == 2 && mv.samples[1] == "M2.WAV");
    const SfxEvent& gun = db.sfx()[db.find_sfx("GUN")];
    CHECK(gun.samples.size() == 4 && gun.probability == 80 && gun.is_3d && gun.priority == 4);
    CHECK(gun.volume_saturation_distance == 200.0f);
    // Defaults (§1.1): Priority 3, Probability 100, Max_Instances 1, pan 50, saturation 300, Is_3D on.
    const SfxEvent& b2 = db.sfx()[db.find_sfx("Bomb_2")];
    CHECK(b2.priority == 3 && b2.probability == 100 && b2.max_instances == 1 && b2.play_count == 1);
    CHECK(b2.min_pan2d == 50 && b2.max_pan2d == 50 && b2.volume_saturation_distance == 300.0f && !b2.is_3d);
    CHECK(db.sfx()[db.find_sfx("Boom3D")].is_3d);
    const SfxEvent& b1 = db.sfx()[db.find_sfx("Bomb_1")];
    CHECK(b1.chained_index == db.find_sfx("Bomb_2") && b1.play_count == 2);
    const SfxEvent& lp = db.sfx()[db.find_sfx("Engine_Move_Loop")];
    CHECK(lp.play_count == -1 && lp.loop_fade_in_seconds == 0.5f && lp.kills_previous_object_sfx);
    CHECK(db.sfx()[db.find_sfx("Broken")].max_volume == 100);

    const MusicEvent& g = db.music()[db.find_music("Galactic")];
    CHECK(g.files.size() == 3 && g.files[1] == "G2.MP3" && g.loop && g.fade_in_seconds == 2.0f && g.volume_percent == 60);
    CHECK(db.speech()[db.find_speech("Line_2")].overlap_test == "Tut");
    const AudioConstants& c = db.constants();
    CHECK(c.saturation_distance_mod[1] == 1.5f && c.rolloff_distance_mod[2] == 10.0f && c.listener_z_pullback[2] == 60.0f);
    CHECK(c.ambient_map_sounds.size() == 1 && c.ambient_map_sounds[0].second == "Ambient_Map_Sounds_Temperate");
    CHECK(c.mss_3d_provider_names.size() == 1 && c.mss_3d_provider_names[0] == "Miles Fast 2D Positional Audio");
    CHECK(c.sfx_gui_negative_feedback == "GUI_Bad_Sound");
    CHECK(db.find_sfx("nothing") == kNoEvent);

    CHECK(localize_file("U000_A_ENG.WAV", "German") == "U000_A_GER.WAV");
    CHECK(localize_file("u000_a_eng.mp3", "Polish") == "u000_a_POL.mp3");
    CHECK(localize_file("U000_A_ENG.WAV", "English") == "U000_A_ENG.WAV");
    CHECK(localize_file("AMB.WAV", "French") == "AMB.WAV");
}

// ------------------------------------------------------------------ SFX rules (§1.2 .. §1.5)

struct Rig {
    MemoryProvider tree = make_tree();
    AudioDatabase db;
    NullBackend backend{test_duration};
    SimRng rng{0x1234u};
    std::unique_ptr<AudioSystem> a;
    explicit Rig(AudioConfig cfg = {}) {
        db.load(tree);
        a = std::make_unique<AudioSystem>(db, backend, rng, cfg);
    }
};

void test_draws() {
    // Gates draw only Probability (int(1,100)); the first update draws volume, pan, pitch, predelay,
    // postdelay in that order, then the primary sample at its stage.
    Rig rig;
    for (int i = 0; i < 40; ++i) {
        SimRng ref = rig.rng;
        const bool pass = ref.range_int(1, 100) <= 80;
        const uint32_t h = rig.a->play_sfx("Gun", at(10.0f));
        CHECK(pass == (h != 0));
        CHECK(rig.rng.state() == ref.state());
        if (!h) {
            CHECK(last_reason(*rig.a) == "probability");
            continue;
        }
        const int32_t vol = ref.range_int(80, 90);
        ref.range_int(50, 50);
        const int32_t pitch = ref.range_int(95, 105);
        const int32_t pick = ref.range_int(0, 3);
        CHECK(rig.a->active_voices(Bus::Sfx3D) == 0);  // nothing plays before the next update
        rig.a->update(0.0);
        const LogEntry* c = last_op(*rig.a, LogOp::SfxCycle);
        CHECK(c && c->volume == vol && c->pitch == pitch && c->pan == 50);
        const LogEntry* v = last_op(*rig.a, LogOp::SfxVoice);
        CHECK(v && v->sample == rig.db.sfx()[rig.db.find_sfx("Gun")].samples[static_cast<size_t>(pick)]);
        CHECK(rig.rng.state() == ref.state());
        const NullBackend::Voice* bv = rig.backend.voice(v->voice);
        CHECK(bv && near(bv->params.pitch, static_cast<float>(pitch) / 100.0f));
        rig.a->update(2.0);  // ends (pitch >= 0.95)
        CHECK(!rig.a->sfx_live(h));
    }
    // 2D pan: drawn 0..100, mapped to -1..+1.
    SimRng ref = rig.rng;
    ref.range_int(100, 100);  // volume: no draw
    const int32_t pan = ref.range_int(0, 100);
    rig.a->play_sfx("Panned");
    rig.a->update(0.0);
    const LogEntry* v = last_op(*rig.a, LogOp::SfxVoice);
    CHECK(v && near(rig.backend.voice(v->voice)->params.pan, static_cast<float>(pan) / 50.0f - 1.0f));
}

void test_gates() {
    Rig rig;
    AudioSystem& a = *rig.a;
    // Max_Instances (gate 6) is tested before Overlap_Test (gate 7).
    const uint32_t h = a.play_sfx("Unit_Select_A");
    CHECK(h != 0);
    CHECK(a.play_sfx("Unit_Select_A") == 0 && last_reason(a) == "max_instances");
    CHECK(a.play_sfx("Unit_Move_A") == 0 && last_reason(a) == "overlap");
    // Sequential primary counter: A1, A2, A3, A1 (no draws).
    std::vector<std::string> picks;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) CHECK(a.play_sfx("Unit_Select_A") != 0);
        a.update(0.0);
        picks.push_back(last_op(a, LogOp::SfxVoice)->sample);
        a.update(1.0);
    }
    CHECK((picks == std::vector<std::string>{"A1_ENG.WAV", "A2_ENG.WAV", "A3_ENG.WAV", "A1_ENG.WAV"}));
    // Presets and Max_Instances 0 never play.
    CHECK(a.play_sfx("Preset_UR") == 0 && last_reason(a) == "preset");
    CHECK(a.play_sfx("Never") == 0 && last_reason(a) == "max_instances_0");
    CHECK(a.play_sfx("NoSuchEvent") == 0 && last_reason(a) == "unknown_event");
    // Permission flags (gate 3).
    a.allow_unit_response_vo(false);
    CHECK(a.play_sfx("Unit_Select_A") == 0 && last_reason(a) == "unit_response_vo_off");
    a.allow_unit_response_vo(true);
    a.allow_localized_sfx(false);
    CHECK(a.play_sfx("Unit_Select_A") == 0 && last_reason(a) == "localized_off");
    a.allow_localized_sfx(true);
    // Negative feedback in a cinematic (gate 2), even forced.
    a.set_cinematic(true);
    SfxPlay forced;
    forced.force = true;
    CHECK(a.play_sfx("GUI_Bad_Sound", forced) == 0 && last_reason(a) == "negative_feedback_in_cinematic");
    a.set_cinematic(false);
    // GUI-only mode (gate 4): only Is_GUI events, never the negative feedback; force skips it.
    a.set_gui_only(true);
    CHECK(a.play_sfx("Bomb_2") == 0 && last_reason(a) == "gui_only");
    CHECK(a.play_sfx("GUI_Bad_Sound") == 0 && last_reason(a) == "gui_only");
    CHECK(a.play_sfx("Click") != 0);
    CHECK(a.play_sfx("Bomb_2", forced) != 0);
    a.set_gui_only(false);
    // HUD VO (gate 5): blocked by the tutorial flag and while speech plays.
    a.set_hud_vo_blocked(true);
    CHECK(a.play_sfx("HUD_Line") == 0 && last_reason(a) == "hud_vo_blocked");
    a.set_hud_vo_blocked(false);
    a.play_speech("Line_1");
    a.update(0.0);
    CHECK(a.speech_playing());
    CHECK(a.play_sfx("HUD_Line") == 0 && last_reason(a) == "hud_vo_blocked");
    a.update(3.0);
    CHECK(!a.speech_playing() && a.play_sfx("HUD_Line") != 0);
    // A looping event already attached to the object (gate 6).
    CHECK(a.play_sfx("Engine_Loop", on_object(5)) != 0);
    CHECK(a.play_sfx("Engine_Loop", on_object(5)) == 0 && last_reason(a) == "loop_already_attached");
    CHECK(a.play_sfx("Engine_Loop", on_object(6)) != 0);
    // Gate 9: 3D-positional starts under fog fail (not in cinematics).
    a.set_position_fogged([](const Vec3& p) { return p.x > 1000.0f; });
    CHECK(a.play_sfx("Gun3D", at(2000.0f)) == 0 && last_reason(a) == "fogged");
    a.set_cinematic(true);
    CHECK(a.play_sfx("Gun3D", at(2000.0f)) != 0);
    // GUI pump: plays in the same frame.
    a.set_cinematic(false);
    SfxPlay pump;
    pump.pump = 1;
    const uint32_t c = a.play_sfx("Click", pump);
    CHECK(c != 0 && voice_of(a, c) != 0);
}

void test_state_machine() {
    {  // Probability 60 + predelay 1000..5000 ms (wall time).
        Rig rig;
        int started = 0, rejected = 0;
        for (int i = 0; i < 200; ++i) {
            SimRng ref = rig.rng;
            const bool pass = ref.range_int(1, 100) <= 60;
            const uint32_t h = rig.a->play_sfx("Chatter");
            CHECK((h != 0) == pass);
            if (!h) {
                ++rejected;
                continue;
            }
            ++started;
            const int32_t delay = ref.range_int(1000, 5000);
            rig.a->update(0.0);
            CHECK(last_op(*rig.a, LogOp::SfxCycle)->predelay_ms == delay);
            CHECK(rig.rng.state() == ref.state());
            CHECK(rig.a->active_voices(Bus::Sfx2D) == 0);
            rig.a->update(static_cast<double>(delay - 1) / 1000.0);
            CHECK(rig.a->active_voices(Bus::Sfx2D) == 0);
            rig.a->update(0.001);
            CHECK(rig.a->active_voices(Bus::Sfx2D) == 1);
            rig.a->update(1.0);
            CHECK(!rig.a->sfx_live(h));
        }
        CHECK(started > 80 && rejected > 40);
    }
    {  // Every cycle is pre -> primary -> post; Play_Count 2 repeats it; then the 2D chain.
        Rig rig;
        rig.a->play_sfx("Bomb_1", on_object(3));
        for (int i = 0; i < 10; ++i) rig.a->update(1.0);
        CHECK((voice_samples(*rig.a) ==
               std::vector<std::string>{"PRE.WAV", "B1.WAV", "POST.WAV", "PRE.WAV", "B1.WAV", "POST.WAV", "B2.WAV"}));
        CHECK(count_op(*rig.a, LogOp::SfxCycle, "Bomb_1") == 2);
        CHECK(count_op(*rig.a, LogOp::SfxChain, "Bomb_2") == 1);
        CHECK(count_op(*rig.a, LogOp::SfxEnd, "Bomb_2") == 1 && rig.a->live_instances() == 0);
        // 3D instances never chain.
        rig.a->clear_log();
        rig.a->play_sfx("Boom3D", at(0.0f));
        for (int i = 0; i < 3; ++i) rig.a->update(1.0);
        CHECK(count_op(*rig.a, LogOp::SfxEnd, "Boom3D") == 1 && count_op(*rig.a, LogOp::SfxChain) == 0);
    }
    {  // Sequential pre/primary/post counters stay aligned.
        Rig rig;
        rig.a->play_sfx("VO_Seq");
        for (int i = 0; i < 4; ++i) rig.a->update(1.0);
        rig.a->play_sfx("VO_Seq");
        for (int i = 0; i < 4; ++i) rig.a->update(1.0);
        CHECK((voice_samples(*rig.a) ==
               std::vector<std::string>{"P1.WAV", "S1.WAV", "Q1.WAV", "P2.WAV", "S2.WAV", "Q2.WAV"}));
    }
    {  // Loop with a per-voice fade-in; stop with a fade-out; Kills_Previous_Object_SFX.
        Rig rig;
        AudioSystem& a = *rig.a;
        SfxPlay p = on_object(9);
        p.fade_in_seconds = 0.5f;
        const uint32_t h = a.play_sfx("Engine_Loop", p);
        a.update(0.0);
        const uint32_t v = voice_of(a, h);
        CHECK(v && rig.backend.voice(v)->params.loop && rig.backend.voice(v)->params.gain == 0.0f);
        a.update(0.25);
        CHECK(near(rig.backend.voice(v)->params.gain, kSfxBus * 0.5f));
        a.update(10.0);  // loops past its 2 s length
        CHECK(a.sfx_live(h) && near(rig.backend.voice(v)->params.gain, kSfxBus));
        a.stop_sfx(h, 1.0f);
        a.update(0.5);
        CHECK(near(rig.backend.voice(v)->params.gain, kSfxBus * 0.5f));
        a.update(0.5);
        CHECK(!a.sfx_live(h) && rig.backend.voice(v) == nullptr);
        const uint32_t h1 = a.play_sfx("Engine_Loop", on_object(9));
        const uint32_t h3 = a.play_sfx("Engine_Loop", on_object(10));
        a.update(0.0);
        const uint32_t h2 = a.play_sfx("Engine_Move_Loop", on_object(9));  // kills everything on object 9
        CHECK(h1 && h2 && h3 && !a.sfx_live(h1) && a.sfx_live(h2) && a.sfx_live(h3));
        a.stop_object_sfx(10, "engine_loop");
        CHECK(!a.sfx_live(h3));
        a.object_removed(9);
        CHECK(!a.sfx_live(h2));
        a.update(0.0);
        CHECK(a.live_instances() == 0 && a.active_voices(Bus::Sfx3D) == 0);
    }
    {  // Missing sample: the stage is skipped.
        MemoryProvider tree = make_tree();
        tree.add("Data\\XML\\SFXEventsTest.xml",
                 R"(<SFXEvents><SFXEvent Name="M"><Is_3D>No</Is_3D><Samples>MISSING.WAV</Samples></SFXEvent></SFXEvents>)");
        AudioDatabase db;
        db.load(tree);
        NullBackend nb{test_duration};
        SimRng rng{1};
        AudioSystem a(db, nb, rng);
        CHECK(a.play_sfx("M") != 0);
        a.update(0.0);
        CHECK(last_op(a, LogOp::SfxVoice)->reason == "sample_missing");
        a.update(0.01);
        CHECK(a.live_instances() == 0);
    }
    {  // Localized samples take the language suffix (captured at instance creation).
        AudioConfig cfg;
        cfg.language = "German";
        Rig rig(cfg);
        rig.a->play_sfx("Unit_Select_A");
        rig.a->update(0.0);
        CHECK(last_op(*rig.a, LogOp::SfxVoice)->sample == "A1_GER.WAV");
    }
}

// ------------------------------------------------------------------ voices, 3D, mixer (§1.6 - §1.8, §4)

void test_voices() {
    {  // 2D: lower Priority number wins; equal priority steals the oldest; less important fails.
        AudioConfig cfg;
        cfg.max_voices_2d = 2;
        Rig rig(cfg);
        AudioSystem& a = *rig.a;
        const uint32_t a1 = a.play_sfx("Ambient_Low");
        a.update(0.1);
        const uint32_t a2 = a.play_sfx("Ambient_Low");
        a.update(0.1);
        CHECK(a.active_voices(Bus::Sfx2D) == 2);
        const uint32_t ur = a.play_sfx("Unit_Select_A");
        a.update(0.0);
        CHECK(voice_of(a, ur) != 0 && last_op(a, LogOp::SfxSteal)->handle == a1);
        const uint32_t a3 = a.play_sfx("Ambient_Low");  // prio 5 <= 5: steals a2 (the oldest prio-5 voice)
        a.update(0.0);
        CHECK(voice_of(a, a3) != 0 && last_op(a, LogOp::SfxSteal)->handle == a2);
        a.update(1.0);
        CHECK(a.live_instances() == 0);
        const uint32_t c1 = a.play_sfx("Click");
        const uint32_t c2 = a.play_sfx("Click");
        a.update(0.0);
        CHECK(c1 && c2 && a.active_voices(Bus::Sfx2D) == 2);
        const uint32_t lo = a.play_sfx("Ambient_Low");
        a.update(0.0);
        CHECK(lo != 0 && last_op(a, LogOp::SfxNoVoice)->handle == lo && !a.sfx_live(lo));
    }
    {  // 3D: steal only a less/equally important voice that is farther than the new sound.
        AudioConfig cfg;
        cfg.max_voices_3d = 2;
        Rig rig(cfg);
        AudioSystem& a = *rig.a;
        const uint32_t g1 = a.play_sfx("Gun3D", at(100.0f));
        a.update(0.0);
        const uint32_t g2 = a.play_sfx("Gun3D", at(200.0f));
        a.update(0.0);
        const uint32_t g3 = a.play_sfx("Gun3D", at(300.0f));
        a.update(0.0);
        CHECK(g1 && g2 && g3 && voice_of(a, g3) == 0 && last_op(a, LogOp::SfxNoVoice)->handle == g3);
        const uint32_t g4 = a.play_sfx("Gun3D", at(50.0f));
        a.update(0.0);
        CHECK(voice_of(a, g4) != 0 && last_op(a, LogOp::SfxSteal)->handle == g2);
    }
    {  // 3D per-event Max_Instances at voice start: refuse a farther one, else stop the farthest.
        Rig rig;
        AudioSystem& a = *rig.a;
        a.play_sfx("Laser", at(100.0f));
        a.update(0.0);
        const uint32_t l2 = a.play_sfx("Laser", at(200.0f));
        a.update(0.0);
        const uint32_t l3 = a.play_sfx("Laser", at(300.0f));  // gate 6 lets 3D through
        CHECK(l3 != 0);
        a.update(0.0);
        CHECK(voice_of(a, l3) == 0);
        const uint32_t l4 = a.play_sfx("Laser", at(150.0f));
        a.update(0.0);
        CHECK(voice_of(a, l4) != 0 && last_op(a, LogOp::SfxSteal)->handle == l2);
    }
    {  // Distance model (§1.7), listener (§1.8), pan.
        Rig rig;
        AudioSystem& a = *rig.a;
        a.set_mode(AudioMode::Land);
        CHECK(a.attenuation(100.0f, 200.0f) == 1.0f);
        CHECK(near(a.attenuation(300.0f, 200.0f), 200.0f / (200.0f + 10.0f * 100.0f), 1e-6f));
        CHECK(near(a.attenuation(10.5f, 0.0f), 1.0f / (1.0f + 10.0f * 9.5f), 1e-6f));  // min clamped to 1
        CHECK(a.attenuation(15001.0f, 200.0f) == 0.0f);                               // beyond max distance
        a.set_mode(AudioMode::Space);  // saturation x1.5, rolloff 2
        CHECK(near(a.attenuation(400.0f, 200.0f), 300.0f / (300.0f + 2.0f * 100.0f), 1e-6f));
        a.set_mode(AudioMode::Land);
        const Listener l = listener_from_camera({0, -500, 400}, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, 60.0f, false);
        CHECK(near(l.position.y, -60.0f) && near(l.position.x, 0.0f) && near(l.position.z, 0.0f));
        const Listener lc = listener_from_camera({0, -500, 400}, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, 60.0f, true);
        CHECK(near(lc.position.y, -500.0f) && near(lc.position.z, 400.0f));
        Listener flat;
        flat.position = {0, 0, 0};
        flat.forward = {0, 1, 0};
        flat.up = {0, 0, 1};
        a.set_listener(flat);
        CHECK(a.pan_for({100, 0, 0}) > 0.99f && a.pan_for({-100, 0, 0}) < -0.99f);
        const uint32_t g = a.play_sfx("Gun3D", at(1000.0f));
        a.update(0.0);
        CHECK(near(last_op(a, LogOp::SfxVoice)->gain, kSfxBus * (200.0f / (200.0f + 10.0f * 800.0f)), 1e-6f));
        // Attached instances follow the object query; hidden objects keep running silently.
        const uint32_t o = a.play_sfx("Engine_Loop", on_object(77));
        a.set_object_query([](uint64_t id) -> std::optional<AudioSystem::ObjectAudio> {
            if (id != 77) return std::nullopt;
            return AudioSystem::ObjectAudio{{0, 0, 0}, false};
        });
        a.update(0.0);
        CHECK(a.sfx_live(o) && rig.backend.voice(voice_of(a, o))->params.gain == 0.0f);
        a.set_3d_scale(0.0f);  // battle-load screen
        a.update(0.0);
        CHECK(rig.backend.voice(voice_of(a, g))->params.gain == 0.0f);
    }
    {  // Buses (§4) and speech ducking (§3.2): localized SFX on the speech bus; SFX capped at 0.3.
        Rig rig;
        AudioSystem& a = *rig.a;
        const uint32_t c = a.play_sfx("Click");
        const uint32_t u = a.play_sfx("Unit_Select_A");
        a.set_bus_level(AudioBus::Speech, 0.5f);
        a.update(0.0);
        CHECK(near(rig.backend.voice(voice_of(a, c))->params.gain, kSfxBus));
        CHECK(near(rig.backend.voice(voice_of(a, u))->params.gain, 0.5f * 0.8f * 0.9f));
        a.set_bus_level(AudioBus::Sfx, 1.0f, 1.0f);  // ramp 0.8 -> 1.0 over 1 s
        a.update(0.5);
        CHECK(near(a.bus_level(AudioBus::Sfx), 0.9f));
        a.set_bus_level(AudioBus::Sfx, 0.8f);
        a.play_speech("Line_1");
        a.update(0.0);  // speech starts after the SFX update
        a.update(0.0);
        CHECK(near(rig.backend.voice(voice_of(a, c))->params.gain, 0.3f));
        a.set_modal_dialog(true);
        a.update(0.0);
        CHECK(near(rig.backend.voice(voice_of(a, c))->params.gain, kSfxBus));
    }
}

// ------------------------------------------------------------------ music (§2) and speech (§3)

void test_music() {
    Rig rig;
    AudioSystem& a = *rig.a;
    const float bus = kSfxBus;  // music bus default 0.8 x master 0.8
    CHECK(a.play_music("Menu"));
    a.update(0.0);
    CHECK(a.current_music() == "Menu" && a.active_voices(Bus::Music) == 1);
    const uint32_t menu_voice = last_op(a, LogOp::MusicTrack)->voice;
    CHECK(near(rig.backend.voice(menu_voice)->params.gain, bus * 0.8f));
    CHECK(!a.play_music("menu") && last_op(a, LogOp::MusicRefuse)->reason == "already_current");
    // Galactic: Menu fades out over Galactic's Fade_Out_Previous_Seconds (1 s), Galactic ramps in over 2 s.
    SimRng ref = rig.rng;
    ref.range_int(0, 2);  // the queue-time draw (unused for the file choice)
    CHECK(a.play_music("Galactic"));
    CHECK(rig.rng.state() == ref.state());
    a.update(0.0);
    CHECK(last_op(a, LogOp::MusicTrack)->sample == "G1.MP3");  // sequential counter from 0
    const uint32_t gal_voice = last_op(a, LogOp::MusicTrack)->voice;
    CHECK(a.active_voices(Bus::Music) == 2);
    a.update(0.5);
    CHECK(near(rig.backend.voice(menu_voice)->params.gain, bus * 0.8f * 0.5f, 1e-4f));
    CHECK(near(rig.backend.voice(gal_voice)->params.gain, bus * 0.6f * 0.25f, 1e-4f));
    a.update(0.5);
    CHECK(rig.backend.voice(menu_voice) == nullptr && a.active_voices(Bus::Music) == 1);
    a.update(1.0);
    CHECK(near(rig.backend.voice(gal_voice)->params.gain, bus * 0.6f, 1e-4f));
    a.update(8.5);  // G1 (10 s) ends: Loop -> next file in order
    CHECK(last_op(a, LogOp::MusicTrack)->sample == "G2.MP3");
    // Sting: no loop, 0 s fades; queued with "resume previous" -> Galactic comes back after it.
    CHECK(a.play_music("Sting", music_mode::kBattlePending, true));
    a.update(0.0);
    CHECK(a.current_music() == "Sting" && a.active_voices(Bus::Music) == 1);
    a.update(10.5);
    CHECK(a.current_music() == "Galactic");
    a.update(0.0);
    CHECK(last_op(a, LogOp::MusicTrack)->sample == "G3.MP3");
    // Mode switches: battle on combat, ambient after the peace time in sim ticks.
    a.set_music_resolver([](uint8_t mode, SimRng&) -> std::string {
        if (mode == music_mode::kTacticalBattle) return "Sting";
        if (mode == music_mode::kTacticalAmbient) return "Menu";
        return "Galactic";
    });
    a.set_mode(AudioMode::Land);
    a.notify_combat(false);
    CHECK(a.current_music() == "Galactic");
    a.notify_combat(true);
    CHECK(a.current_music() == "Sting" && a.music_mode() == music_mode::kTacticalBattle);
    for (int t = 0; t < 449; ++t) a.music_sim_tick();
    CHECK(a.music_mode() == music_mode::kTacticalBattle);
    a.notify_combat(true);  // resets the peace counter
    for (int t = 0; t < 449; ++t) a.music_sim_tick();
    CHECK(a.music_mode() == music_mode::kTacticalBattle);
    a.music_sim_tick();
    CHECK(a.current_music() == "Menu" && a.music_mode() == music_mode::kTacticalAmbient);
    // Lua Play_Music is mode 12; automatic mode music does nothing there unless forced.
    CHECK(a.play_music("Sting", music_mode::kLua, false, true));
    a.set_base_music_mode(music_mode::kGalactic);
    a.resume_mode_based_music(false);
    CHECK(a.current_music() == "Sting");
    a.resume_mode_based_music(true);
    CHECK(a.current_music() == "Galactic" && a.music_mode() == music_mode::kGalactic);
    a.update(0.0);
    a.stop_all_music();
    CHECK(a.active_voices(Bus::Music) == 0 && a.current_music().empty());
}

void test_speech() {
    Rig rig;
    AudioSystem& a = *rig.a;
    std::vector<std::string> done;
    a.on_speech_done([&](const std::string& ev, bool killed) { done.push_back(ev + (killed ? "!" : "")); });
    CHECK(a.play_speech("Line_1"));
    CHECK(a.play_speech("Line_2"));
    CHECK(a.play_speech("Line_3"));  // Overlap_Test is data only for speech (§3.1)
    CHECK(!a.speech_playing());     // starts on the next update
    a.update(0.0);
    CHECK(a.current_speech() == "Line_1" && a.speech_queue_size() == 3);
    const LogEntry* s = last_op(a, LogOp::SpeechStart);
    CHECK(s && s->sample == "L1_ENG.MP3" && near(s->gain, 0.8f * 0.8f * 0.9f));
    a.update(3.0);
    CHECK(!a.speech_playing() && done.size() == 1 && done[0] == "Line_1");
    a.update(0.0);  // the next entry starts on the next update
    CHECK(a.current_speech() == "Line_2");
    a.stop_all_speech();
    CHECK((done == std::vector<std::string>{"Line_1", "Line_2!", "Line_3!"}));
    CHECK(a.speech_queue_size() == 0 && a.active_voices(Bus::Speech) == 0);
}

// Scripted session: identical inputs -> identical logs; only the free RNG instance is drawn.
std::string scripted_session(uint32_t seed, GameRandom& gr) {
    MemoryProvider tree = make_tree();
    AudioDatabase db;
    db.load(tree);
    NullBackend nb{test_duration};
    gr.seed_free(seed);
    AudioConfig cfg;
    cfg.max_voices_3d = 4;
    AudioSystem a(db, nb, gr.free(), cfg);
    a.play_music("Galactic");
    for (int f = 0; f < 300; ++f) {
        if (f % 3 == 0) a.play_sfx("Gun", at(static_cast<float>(f)));
        if (f % 17 == 0) a.play_sfx("Chatter");
        if (f % 40 == 0) a.play_sfx("Unit_Select_A");
        if (f == 100) a.play_music("Menu");
        if (f == 150) a.play_speech("Line_1");
        a.update(1.0 / 30.0);
    }
    return a.log_text();
}

void test_determinism() {
    GameRandom g1(777, 0), g2(777, 0);
    const std::string l1 = scripted_session(42, g1);
    const std::string l2 = scripted_session(42, g2);
    CHECK(!l1.empty() && l1 == l2);
    CHECK(g1.synced().state() == 777 && g1.synced().draws() == 0);  // never touched
    CHECK(g1.free().draws() > 0);
    GameRandom g3(777, 0);
    CHECK(scripted_session(43, g3) != l1);
    std::printf("determinism: %zu log bytes, %llu free draws\n", l1.size(),
                static_cast<unsigned long long>(g1.free().draws()));
}

// ------------------------------------------------------------------ decode + miniaudio backend

std::vector<uint8_t> make_wav(uint32_t rate, uint32_t frames, float hz) {
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    };
    const uint32_t data = frames * 2;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    u32(36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    u32(data);
    for (uint32_t i = 0; i < frames; ++i) {
        const double s = std::sin(6.283185307179586 * hz * i / rate) * 16000.0;
        u16(static_cast<uint16_t>(static_cast<int16_t>(s)));
    }
    return b;
}

class MemSamples final : public SampleSource {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool read(const SampleRef& r, std::vector<uint8_t>& out) const override {
        auto it = files.find(normalize_name(r.file));
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(const SampleRef& r) const override { return files.count(normalize_name(r.file)) != 0; }
    std::string origin(const SampleRef& r) const override { return exists(r) ? "mem" : ""; }
};

void test_decode_backend() {
    const std::vector<uint8_t> wav = make_wav(22050, 2205, 441.0f);  // 0.1 s
    CHECK(sniff_container(wav) == Container::Wav);
    auto info = probe_audio(wav);
    CHECK(info && info->channels == 1 && info->sample_rate == 22050 && info->frames == 2205 && info->wav_format_tag == 1);
    PcmBuffer pcm;
    CHECK(decode_audio(wav, pcm) && pcm.frames() == 2205);
    CHECK(std::fabs(pcm.samples[25] - static_cast<float>(std::sin(6.283185307179586 * 441.0 * 25 / 22050) * 16000.0 / 32768.0)) < 1e-3f);
    std::vector<uint8_t> junk = {1, 2, 3, 4, 5};
    CHECK(!probe_audio(junk) && sniff_container(junk) == Container::Unknown);

    MemSamples ms;
    ms.files[normalize_name("TONE.WAV")] = wav;
    std::string err;
    MiniaudioConfig mc;
    mc.no_device = true;
    mc.sample_rate = 44100;
    auto be = MiniaudioBackend::create(ms, mc, &err);
    CHECK(be != nullptr);
    if (!be) {
        std::printf("miniaudio: %s\n", err.c_str());
        return;
    }
    VoiceParams vp;
    vp.gain = 0.5f;
    CHECK(be->start(1, SampleRef{Bus::Sfx2D, "tone.wav"}, vp));
    CHECK(!be->start(2, SampleRef{Bus::Sfx2D, "none.wav"}, vp));
    CHECK(be->playing(1) && be->cached_samples() == 1);
    std::vector<float> mix(4410 * 2);
    CHECK(be->read_mix(mix.data(), 4410) == 4410);
    float peak = 0.0f;
    for (float s : mix) peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.1f && peak < 0.3f);  // 16000/32768 x 0.5 gain x pan law
    CHECK(be->read_mix(mix.data(), 4410) == 4410);  // 0.2 s pulled > 0.1 s tone
    CHECK(!be->playing(1));
    // Through the AudioSystem: the same rules drive the real backend.
    MemoryProvider tree = make_tree();
    tree.add("Data\\XML\\SFXEventsTest.xml", R"(<SFXEvents><SFXEvent Name="Tone"><Samples>TONE.WAV</Samples><Min_Pitch>200</Min_Pitch><Max_Pitch>200</Max_Pitch></SFXEvent></SFXEvents>)");
    AudioDatabase db;
    db.load(tree);
    SimRng rng{5};
    AudioSystem a(db, *be, rng);
    CHECK(a.play_sfx("Tone") != 0);
    a.update(0.0);
    be->read_mix(mix.data(), 2205);  // 0.05 s at 2x pitch = the whole 0.1 s tone
    be->read_mix(mix.data(), 441);
    a.update(0.06);
    CHECK(a.live_instances() == 0 && count_op(a, LogOp::SfxEnd, "Tone") == 1);
}

// ------------------------------------------------------------------ streaming PCM voices (movie sound)

// Synthetic producer like MoviePlayer::read_audio: a sine, interleaved, `available` frames at most.
struct SineSource {
    uint32_t rate = 22050, channels = 1;
    double hz = 441.0;
    uint64_t produced = 0;
    uint64_t available = ~uint64_t{0};
    float value(uint64_t frame, uint32_t ch) const {
        return static_cast<float>(0.5 * std::sin(6.283185307179586 * hz * static_cast<double>(frame) / rate)) *
               (ch == 0 ? 1.0f : -1.0f);
    }
    size_t read(float* out, size_t frames) {
        const uint64_t n = std::min<uint64_t>(frames, available - std::min(available, produced));
        for (uint64_t f = 0; f < n; ++f)
            for (uint32_t c = 0; c < channels; ++c) out[f * channels + c] = value(produced + f, c);
        produced += n;
        return static_cast<size_t>(n);
    }
};

void test_streams() {
    // Resampler alone: 22050 mono -> 44100 stereo = every source sample at even outputs, midpoints between.
    {
        SineSource src;
        StreamResampler rs({22050, 1}, 44100, 2, [&](float* o, size_t n) { return src.read(o, n); });
        std::vector<float> out(4410 * 2);
        CHECK(rs.read(out.data(), 4410) == 4410);
        CHECK(rs.source_frames_pulled() <= 2205 + 1 && rs.source_frames_pulled() >= 2205);  // <= 1 frame lookahead
        bool ok = true;
        for (size_t i = 0; i + 1 < 2204; ++i) {
            ok &= near(out[2 * i * 2], src.value(i, 0)) && near(out[2 * i * 2 + 1], src.value(i, 0));  // mono -> both
            ok &= near(out[(2 * i + 1) * 2], 0.5f * (src.value(i, 0) + src.value(i + 1, 0)));
        }
        CHECK(ok);
        // 48000 stereo -> 44100: output follows the analytic sine at the device rate (linear interp error).
        SineSource s48;
        s48.rate = 48000;
        s48.channels = 2;
        StreamResampler r2({48000, 2}, 44100, 2, [&](float* o, size_t n) { return s48.read(o, n); });
        std::vector<float> o2(44100 * 2);
        r2.read(o2.data(), 22050);
        r2.read(o2.data() + 22050 * 2, 22050);  // two blocks: state carries across calls
        float err = 0.0f;
        for (size_t i = 0; i < 44100; ++i) {
            const float want = static_cast<float>(0.5 * std::sin(6.283185307179586 * 441.0 * static_cast<double>(i) / 44100.0));
            err = std::max(err, std::max(std::fabs(o2[i * 2] - want), std::fabs(o2[i * 2 + 1] + want)));
        }
        CHECK(err < 1e-3f);
        CHECK(s48.produced >= 48000 && s48.produced <= 48001);
        // Stereo -> mono averages.
        SineSource st;
        st.channels = 2;
        st.rate = 44100;
        StreamResampler r3({44100, 2}, 44100, 1, [&](float* o, size_t n) { return st.read(o, n); });
        std::vector<float> o3(100);
        r3.read(o3.data(), 100);
        CHECK(near(o3[17], 0.0f) && st.produced == 100);  // channels are opposite: average 0
        // Underrun: silence, position kept, resumes with the next sample.
        SineSource u;
        u.rate = 44100;
        u.available = 50;
        StreamResampler r4({44100, 1}, 44100, 1, [&](float* o, size_t n) { return u.read(o, n); });
        std::vector<float> o4(100, 9.0f);
        CHECK(r4.read(o4.data(), 100) == 50 && o4[60] == 0.0f && r4.underrun_frames() == 50);
        u.available = 1000;
        r4.read(o4.data(), 10);
        CHECK(near(o4[0], u.value(50, 0)));
        SineSource u2;
        u2.available = 20;  // 22050 -> 44100 underrun
        StreamResampler r5({22050, 1}, 44100, 1, [&](float* o, size_t n) { return u2.read(o, n); });
        std::vector<float> o5(100);
        const size_t got5 = r5.read(o5.data(), 100);
        CHECK(got5 == 38 && near(o5[37], 0.5f * (u2.value(18, 0) + u2.value(19, 0))) && o5[38] == 0.0f);
        u2.available = 1000;
        r5.read(o5.data(), 4);
        CHECK(near(o5[0], u2.value(19, 0)));  // continues where it stopped
    }
    // Through AudioSystem + NullBackend (device 44100 stereo).
    {
        Rig rig;
        AudioSystem& a = *rig.a;
        SineSource movie;
        AudioSystem::StreamParams sp;
        sp.format = {22050, 1};
        const uint32_t h = a.open_stream(sp, [&](float* o, size_t n) { return movie.read(o, n); });
        CHECK(h != 0 && a.stream_open(h) && a.open_streams() == 1);
        const LogEntry* op = last_op(a, LogOp::StreamOpen);
        CHECK(op && op->handle == h && near(op->gain, kSfxBus));  // speech bus 0.8 x master 0.8
        const uint32_t stream_voice = op ? op->voice : 0;  // Int-final: update() may reallocate the log (`op` dangles)
        a.update(0.1);
        const NullBackend::StreamVoice* sv = rig.backend.stream(stream_voice);
        CHECK(sv && sv->device_frames == 4410 && movie.produced >= 2205 && movie.produced <= 2206);
        CHECK(sv->captured.size() == 4410 * 2 && near(sv->captured[2 * 2], movie.value(1, 0) * kSfxBus));
        // Ducking: an SFX voice is capped at 0.3 while the stream is open (as during speech).
        CHECK(a.play_sfx("Bomb_2") != 0);
        a.update(0.0);
        const LogEntry* bv = last_op(a, LogOp::SfxVoice);
        CHECK(bv && bv->gain <= 0.3f + 1e-6f);
        // Volume ramp over 1 s of wall time.
        a.set_stream_volume(h, 0.0f, 1.0f);
        a.update(0.5);
        CHECK(near(a.stream_gain(h), kSfxBus * 0.5f, 1e-3f));
        a.update(0.5);
        CHECK(a.stream_gain(h) == 0.0f);
        a.set_stream_volume(h, 1.0f);
        // stop_all_sfx / stop_all_speech leave it alone; stop mid-stream stops pulling at once.
        a.stop_all_sfx();
        a.stop_all_speech();
        CHECK(a.stream_open(h));
        const uint64_t before = movie.produced;
        a.update(0.05);
        CHECK(movie.produced > before);
        a.stop_stream(h);
        const uint64_t at_stop = movie.produced;
        a.update(0.5);
        CHECK(movie.produced == at_stop && !a.stream_open(h) && rig.backend.stream(op->voice) == nullptr);
        CHECK(last_op(a, LogOp::StreamStop) && last_op(a, LogOp::StreamStop)->handle == h);
        // Without streams the SFX cap is gone again.
        a.update(0.0);
        CHECK(a.play_sfx("Click") != 0);
        a.update(0.0);
        CHECK(last_op(a, LogOp::SfxVoice)->gain > 0.3f);
        // Clock: device consumption over many small irregular updates = round(total x rate).
        SineSource m2;
        m2.rate = 44100;
        const uint32_t h2 = a.open_stream(44100, 1, AudioBus::Music, [&](float* o, size_t n) { return m2.read(o, n); });
        for (int i = 0; i < 100; ++i) a.update(i % 2 ? 0.013 : 0.0217);
        CHECK(m2.produced == static_cast<uint64_t>(std::floor((0.013 + 0.0217) * 50 * 44100 + 1e-6)));
        a.stop_stream(h2);
        CHECK(a.open_stream(sp, nullptr) == 0 && last_op(a, LogOp::StreamOpen)->reason == "bad_format");
    }
    // A backend without stream support refuses.
    {
        struct NoStreams final : public VoiceBackend {
            bool start(uint32_t, const SampleRef&, const VoiceParams&) override { return false; }
            void set(uint32_t, const VoiceParams&) override {}
            void stop(uint32_t) override {}
            bool playing(uint32_t) const override { return false; }
            void advance(double) override {}
        } ns;
        MemoryProvider tree = make_tree();
        AudioDatabase db;
        db.load(tree);
        SimRng rng{1};
        AudioSystem a(db, ns, rng);
        CHECK(a.open_stream(22050, 1, AudioBus::Speech, [](float*, size_t) { return size_t{0}; }) == 0);
        CHECK(last_op(a, LogOp::StreamOpen)->reason == "unsupported");
    }
    // miniaudio in no-device mode: the stream is mixed at the engine rate and pulled as the mix is read.
    {
        MemSamples ms;
        MiniaudioConfig mc;
        mc.no_device = true;
        mc.sample_rate = 44100;
        auto be = MiniaudioBackend::create(ms, mc);
        CHECK(be != nullptr);
        if (!be) return;
        MemoryProvider tree = make_tree();
        AudioDatabase db;
        db.load(tree);
        SimRng rng{1};
        AudioSystem a(db, *be, rng);
        SineSource movie;
        const uint32_t h = a.open_stream(22050, 1, AudioBus::Speech, [&](float* o, size_t n) { return movie.read(o, n); });
        CHECK(h != 0);
        std::vector<float> mix(4410 * 2);
        CHECK(be->read_mix(mix.data(), 4410) == 4410);
        float peak = 0.0f;
        for (float s : mix) peak = std::max(peak, std::fabs(s));
        CHECK(peak > 0.1f && peak < 0.5f * kSfxBus + 1e-3f);
        // miniaudio's node graph reads its sounds up to one processing chunk ahead of the mix (measured 100..223
        // source frames); the lead stays bounded, the pull rate follows consumption (no drift).
        const uint64_t lead1 = movie.produced - 2205;
        CHECK(movie.produced >= 2205 && lead1 <= 512);
        for (int i = 0; i < 9; ++i) be->read_mix(mix.data(), 4410);
        const uint64_t lead10 = movie.produced - 22050;
        std::printf("streams: miniaudio lead %llu / %llu source frames after 0.1 / 1.0 s\n",
                    static_cast<unsigned long long>(lead1), static_cast<unsigned long long>(lead10));
        CHECK(movie.produced >= 22050 && lead10 <= 512);
        a.stop_stream(h);
        const uint64_t at_stop = movie.produced;
        be->read_mix(mix.data(), 4410);
        CHECK(movie.produced == at_stop);
    }
}

// ------------------------------------------------------------------ Lua presentation bridge

#ifdef ALAMO_AUDIO_HAVE_LUA
void test_lua_bridge() {
    Rig rig;
    using lua::PresentationArg;
    using lua::PresentationEvent;
    using lua::PresentationOp;
    auto str = [](const char* s) {
        PresentationArg a;
        a.kind = PresentationArg::Kind::String;
        a.text = s;
        return a;
    };
    auto obj = [](int32_t id) {
        PresentationArg a;
        a.kind = PresentationArg::Kind::Object;
        a.id = id;
        return a;
    };
    auto boolean = [](bool v) {
        PresentationArg a;
        a.kind = PresentationArg::Kind::Bool;
        a.b = v;
        return a;
    };
    PresentationArg mgr;
    mgr.kind = PresentationArg::Kind::Other;
    mgr.text = "LuaSFXCommandsClassClass";
    std::vector<PresentationEvent> ev;
    auto add = [&](PresentationOp op, std::vector<PresentationArg> args) {
        PresentationEvent e;
        e.seq = static_cast<uint32_t>(ev.size());
        e.op = op;
        e.args = std::move(args);
        ev.push_back(std::move(e));
    };
    add(PresentationOp::PlayMusic, {str("Menu")});
    add(PresentationOp::SfxAllowUnitResponseVo, {mgr, boolean(false)});
    add(PresentationOp::ObjectPlaySfxEvent, {obj(12), str("Unit_Select_A")});
    add(PresentationOp::SfxAllowUnitResponseVo, {mgr, boolean(true)});
    add(PresentationOp::ObjectPlaySfxEvent, {obj(12), str("Engine_Loop")});
    add(PresentationOp::FadeScreenIn, {});
    add(PresentationOp::ResumeModeBasedMusic, {});
    add(PresentationOp::PlayMusic, {});
    uint32_t next = 0;
    LuaAudioResult r = apply_presentation_audio(*rig.a, ev, &next, [](int32_t id) -> std::optional<Vec3> {
        return Vec3{static_cast<float>(id), 0, 0};
    });
    CHECK(r.applied == 6 && r.ignored == 1 && r.unhandled == 0 && r.bad_args == 1 && next == 8);
    CHECK(rig.a->current_music() == "Menu");
    CHECK(last_op(*rig.a, LogOp::SfxReject)->reason == "unit_response_vo_off");
    CHECK(rig.a->live_instances() == 1 && last_op(*rig.a, LogOp::SfxStart)->owner == 12);
    add(PresentationOp::ObjectStopSfxEvent, {obj(12), str("Engine_Loop")});
    r = apply_presentation_audio(*rig.a, ev, &next);
    CHECK(r.applied == 1 && next == 9);
    rig.a->update(2.0);
    CHECK(rig.a->live_instances() == 0);
}
#endif

// ------------------------------------------------------------------ real data

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void write_report(const std::string& game, const std::string& text) {
#ifdef ALAMO_AUDIO_LOG_DIR
    std::ofstream f(std::string(ALAMO_AUDIO_LOG_DIR) + "/audio_" + game + "_report.txt", std::ios::binary);
    f << text;
#else
    (void)game;
    (void)text;
#endif
}

void test_real(const std::string& game, const std::string& root, bool is_eaw) {
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (is_eaw) cfg.eaw_root = std::string();
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) return;
    AudioDatabase db;
    AudioLoadReport r;
    CHECK(db.load(*v, &r));
    std::string rep = "=== " + game + " (" + root + ")\n" + r.to_string();
    std::printf("real data %s: sfx %u (presets %u) music %u speech %u, unknown tags %zu, diagnostics %zu\n",
                game.c_str(), r.sfx_events, r.sfx_presets, r.music_events, r.speech_events, r.unknown_tags.size(),
                r.diagnostics.size());
    CHECK(r.sfx_events > 1500 && r.music_events > 40 && r.speech_events > 500 && r.sfx_files_missing == 0);
    CHECK(r.unresolved_presets == 0);
    rep += "diagnostics:\n";
    for (const std::string& d : r.diagnostics) rep += "  " + d + "\n";

    GameSampleConfig sc;
    sc.sfx_megs = GameSampleSource::default_sfx_megs(*v, "English");
    GameSampleSource src(*v, sc);
    rep += "sfx archives:";
    for (const std::string& m : sc.sfx_megs) rep += " " + m;
    rep += "\n";
    CHECK(src.archive_count() >= 3);

    // Resolve every referenced file once.
    std::set<std::string> seen;
    uint32_t sfx_refs = 0, sfx_missing = 0, music_refs = 0, music_missing = 0, speech_refs = 0, speech_missing = 0;
    std::vector<SampleRef> sample_wav, sample_music, sample_speech;
    std::string missing_list;
    for (const SfxEvent& e : db.sfx()) {
        const Bus bus = e.is_3d ? Bus::Sfx3D : Bus::Sfx2D;
        for (const auto* list : {&e.samples, &e.pre_samples, &e.post_samples})
            for (const std::string& s : *list) {
                if (!seen.insert("S" + normalize_name(s)).second) continue;
                ++sfx_refs;
                SampleRef ref{bus, s};
                if (!src.exists(ref)) {
                    ++sfx_missing;
                    if (sfx_missing <= 40) missing_list += "  sfx " + e.name + ": " + s + "\n";
                } else if (sfx_refs % 25 == 0) {
                    sample_wav.push_back(ref);
                }
            }
    }
    for (const MusicEvent& m : db.music())
        for (const std::string& f : m.files) {
            if (!seen.insert("M" + normalize_name(f)).second) continue;
            ++music_refs;
            SampleRef ref{Bus::Music, f};
            if (!src.exists(ref)) {
                ++music_missing;
                missing_list += "  music " + m.name + ": " + f + "\n";
            } else if (music_refs % 8 == 0) {
                sample_music.push_back(ref);
            }
        }
    for (const SpeechEvent& s : db.speech())
        for (const std::string& f : s.files) {
            if (!seen.insert("P" + normalize_name(f)).second) continue;
            ++speech_refs;
            SampleRef ref{Bus::Speech, f};
            if (!src.exists(ref)) {
                ++speech_missing;
                if (speech_missing <= 40) missing_list += "  speech " + s.name + ": " + f + "\n";
            } else if (speech_refs % 60 == 0) {
                sample_speech.push_back(ref);
            }
        }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "referenced files: sfx %u (missing %u), music %u (missing %u), speech %u (missing %u)\n", sfx_refs,
                  sfx_missing, music_refs, music_missing, speech_refs, speech_missing);
    std::printf("real data %s: %s", game.c_str(), buf);
    rep += buf;
    rep += "missing (first 40 per kind):\n" + missing_list;
    // Most references resolve (the shipped data has a few dead ones).
    CHECK(sfx_missing * 20 < sfx_refs);
    CHECK(music_missing * 10 < music_refs + 10);

    // Probe a sample of each kind; fully decode the first few.
    std::map<std::string, uint32_t> formats;
    uint32_t probed = 0, failed = 0, decoded = 0;
    double total_seconds = 0.0;
    for (const auto* list : {&sample_wav, &sample_music, &sample_speech})
        for (const SampleRef& ref : *list) {
            std::vector<uint8_t> bytes;
            if (!src.read(ref, bytes)) {
                ++failed;
                continue;
            }
            std::string perr;
            auto info = probe_audio(bytes, &perr);
            if (!info) {
                ++failed;
                rep += "  undecodable " + ref.file + ": " + perr + "\n";
                continue;
            }
            ++probed;
            total_seconds += info->seconds();
            std::snprintf(buf, sizeof buf, "%s fmt=%u ch=%u rate=%u", info->container == Container::Wav ? "WAV" : "MP3",
                          info->wav_format_tag, info->channels, info->sample_rate);
            ++formats[buf];
            if (decoded < 6) {
                PcmBuffer pcm;
                if (decode_audio(bytes, pcm)) {
                    ++decoded;
                    CHECK(pcm.frames() > 0 && pcm.frames() + 2048 >= info->frames);
                } else {
                    ++failed;
                }
            }
        }
    std::snprintf(buf, sizeof buf, "probed %u files (%.1f s of audio), %u failed, %u fully decoded\nformats:\n", probed,
                  total_seconds, failed, decoded);
    rep += buf;
    for (const auto& [k, n] : formats) rep += "  " + k + ": " + std::to_string(n) + "\n";
    std::printf("real data %s: probed %u (failed %u), %zu formats\n", game.c_str(), probed, failed, formats.size());
    CHECK(probed > 50 && failed == 0);

    // A short null-device session over real events with real lengths.
    NullBackend nb([&](const SampleRef& ref) -> std::optional<double> {
        std::vector<uint8_t> bytes;
        if (!src.read(ref, bytes)) return std::nullopt;
        auto info = probe_audio(bytes);
        if (!info) return std::nullopt;
        return info->seconds();
    });
    SimRng rng{2026};
    AudioSystem a(db, nb, rng);
    uint32_t started = 0;
    for (size_t i = 0; i < db.sfx().size(); i += 97)
        if (!db.sfx()[i].is_preset) started += a.play_sfx(static_cast<uint32_t>(i), at(0.0f)) ? 1u : 0u;
    if (!db.music().empty()) a.play_music(db.music().front().name);
    for (int f = 0; f < 90; ++f) a.update(1.0 / 30.0);
    std::snprintf(buf, sizeof buf, "null session: %u instances started, %zu log lines\n", started, a.log().size());
    rep += buf;
    CHECK(started > 5);
    write_report(game, rep);
}

}  // namespace

int main() {
    test_parse();
    test_draws();
    test_gates();
    test_state_machine();
    test_voices();
    test_music();
    test_speech();
    test_determinism();
    test_decode_backend();
    test_streams();
#ifdef ALAMO_AUDIO_HAVE_LUA
    test_lua_bridge();
#else
    std::printf("lua bridge: lua module not configured, skipped\n");
#endif
    const std::string foc = env("ALAMO_FOC_DATA"), eaw = env("ALAMO_EAW_DATA");
    if (foc.empty() && eaw.empty()) std::printf("real data: ALAMO_FOC_DATA / ALAMO_EAW_DATA not set, skipped\n");
    if (!foc.empty()) test_real("foc", foc, false);
    if (!eaw.empty()) test_real("eaw", eaw, true);
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_audio_tests: all passed\n");
    return 0;
}
