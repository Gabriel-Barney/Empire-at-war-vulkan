// alamo_audio_game_tests: the game-host layer of engine/audio (no framework; non-zero exit on failure).
//   * GameAudioData parsing (WeatherAudio.xml, AmbientMapSounds.xml, faction audio tags) and selection rules;
//   * AmbienceManager: weather loop crossfades, intermittents 50 units around the listener, map
//     intermittents only in clear weather, map beds, Weather_Audio_Pause, retry after stop_all;
//   * ConversationPlayer: line order, pauses, priority interrupts, speech ducking / HUD VO rule during lines;
//   * GameAudio hooks: mode music (galactic / pending / load screen / ambient / battle / peace / super weapon /
//     win-lose / front end), listener from the camera, unit responses, battle events, object loops and
//     chatter, base-attack announcement rate limit, weather announcements, enemy spotted, GUI pumping,
//     leave_mode, Lua presentation ops through GameAudio, determinism (free RNG only);
//   * real data (ALAMO_FOC_DATA / ALAMO_EAW_DATA; skipped when unset): every weather / ambient / faction
//     audio reference resolved against the SFX and music databases, per-faction mode music for every
//     environment, unit responses of every shipped unit type, a weather session with real sample lengths,
//     the shipped weather loop decoded. Report: logs/audio_game_<game>_report.txt.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "alamo/audio/ambience.hpp"
#include "alamo/audio/audio_db.hpp"
#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/conversation.hpp"
#include "alamo/audio/decode.hpp"
#include "alamo/audio/game_audio.hpp"
#include "alamo/audio/game_audio_data.hpp"
#ifdef ALAMO_AUDIO_HAVE_LUA
#include "alamo/audio/lua_audio.hpp"
#endif
#include "alamo/audio/sample_source.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::audio;

using ::alamo::test::g_fail;

namespace {

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

const char* kSfx = R"(<?xml version="1.0"?>
<SFXEvents>
  <SFXEvent Name="Preset_AMB_2D_Loop"><Is_Preset>Yes</Is_Preset><Is_3D>No</Is_3D><Is_2D>Yes</Is_2D><Priority>1</Priority>
    <Max_Instances>2</Max_Instances><Play_Count>-1</Play_Count><Min_Volume>55</Min_Volume><Max_Volume>55</Max_Volume>
    <Loop_Fade_In_Seconds>3.0</Loop_Fade_In_Seconds><Loop_Fade_Out_Seconds>7.0</Loop_Fade_Out_Seconds></SFXEvent>
  <SFXEvent Name="Preset_AMB_3D"><Is_Preset>Yes</Is_Preset><Is_3D>Yes</Is_3D><Priority>5</Priority><Max_Instances>3</Max_Instances>
    <Volume_Saturation_Distance>200.0</Volume_Saturation_Distance></SFXEvent>
  <SFXEvent Name="Preset_UR"><Is_Preset>Yes</Is_Preset><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_Unit_Response_VO>Yes</Is_Unit_Response_VO>
    <Priority>1</Priority><Localize>Yes</Localize></SFXEvent>
  <SFXEvent Name="Preset_HUD"><Is_Preset>Yes</Is_Preset><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_HUD_VO>Yes</Is_HUD_VO>
    <Priority>1</Priority><Localize>Yes</Localize></SFXEvent>
  <SFXEvent Name="Preset_LOOP3D"><Is_Preset>Yes</Is_Preset><Is_3D>Yes</Is_3D><Priority>2</Priority><Max_Instances>8</Max_Instances>
    <Play_Count>-1</Play_Count><Loop_Fade_In_Seconds>0.5</Loop_Fade_In_Seconds><Loop_Fade_Out_Seconds>1.0</Loop_Fade_Out_Seconds></SFXEvent>

  <SFXEvent Name="Rain_Heavy_Loop"><Use_Preset>Preset_AMB_2D_Loop</Use_Preset><Samples>RAIN_H.WAV</Samples></SFXEvent>
  <SFXEvent Name="Rain_Light_Loop"><Use_Preset>Preset_AMB_2D_Loop</Use_Preset><Samples>RAIN_L.WAV</Samples></SFXEvent>
  <SFXEvent Name="Thunder"><Use_Preset>Preset_AMB_3D</Use_Preset><Samples>TH1.WAV TH2.WAV</Samples></SFXEvent>
  <SFXEvent Name="Birds"><Use_Preset>Preset_AMB_3D</Use_Preset><Samples>B1.WAV</Samples></SFXEvent>
  <SFXEvent Name="Bed_Cricket"><Use_Preset>Preset_AMB_3D</Use_Preset><Samples>CR.WAV</Samples></SFXEvent>

  <SFXEvent Name="Unit_Select"><Use_Preset>Preset_UR</Use_Preset><Samples>S1_ENG.WAV</Samples><Overlap_Test>U</Overlap_Test></SFXEvent>
  <SFXEvent Name="Unit_Move"><Use_Preset>Preset_UR</Use_Preset><Samples>M1_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="Unit_Group_Move"><Use_Preset>Preset_UR</Use_Preset><Samples>GM1_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="Unit_Attack_Ability"><Use_Preset>Preset_UR</Use_Preset><Samples>AA1_ENG.WAV</Samples></SFXEvent>

  <SFXEvent Name="Engine_Idle"><Use_Preset>Preset_LOOP3D</Use_Preset><Samples>IDLE.WAV</Samples></SFXEvent>
  <SFXEvent Name="Preset_EGLC_Idle"><Is_Preset>Yes</Is_Preset><Is_3D>Yes</Is_3D><Priority>1</Priority><Max_Instances>2</Max_Instances>
    <Play_Count>-1</Play_Count><Min_Volume>70</Min_Volume><Max_Volume>70</Max_Volume><Volume_Saturation_Distance>800</Volume_Saturation_Distance>
    <Loop_Fade_In_Seconds>0.5</Loop_Fade_In_Seconds><Loop_Fade_Out_Seconds>1.0</Loop_Fade_Out_Seconds></SFXEvent>
  <SFXEvent Name="Preset_EGLC"><Is_Preset>Yes</Is_Preset><Is_3D>Yes</Is_3D><Priority>1</Priority><Max_Instances>2</Max_Instances>
    <Play_Count>-1</Play_Count><Min_Volume>80</Min_Volume><Max_Volume>80</Max_Volume><Volume_Saturation_Distance>800</Volume_Saturation_Distance>
    <Loop_Fade_In_Seconds>0.5</Loop_Fade_In_Seconds><Loop_Fade_Out_Seconds>1.0</Loop_Fade_Out_Seconds></SFXEvent>
  <SFXEvent Name="EGL_Idle"><Use_Preset>Preset_EGLC_Idle</Use_Preset><Samples>EGL_SD.WAV</Samples></SFXEvent>
  <SFXEvent Name="EGL_Move"><Use_Preset>Preset_EGLC</Use_Preset><Samples>EGL_SD.WAV</Samples></SFXEvent>
  <SFXEvent Name="EGL_Focus"><Use_Preset>Preset_EGLC</Use_Preset><Samples>EGL_F.WAV</Samples></SFXEvent>
  <SFXEvent Name="EGL_Never"><Use_Preset>Preset_EGLC_Idle</Use_Preset><Samples>EGL_SD.WAV</Samples><Probability>0</Probability></SFXEvent>
  <SFXEvent Name="Engine_Move"><Use_Preset>Preset_LOOP3D</Use_Preset><Samples>MOVE.WAV</Samples></SFXEvent>
  <SFXEvent Name="Ambient_Hum"><Use_Preset>Preset_LOOP3D</Use_Preset><Samples>HUM.WAV</Samples></SFXEvent>
  <SFXEvent Name="Chatter_Moving"><Samples>CH.WAV</Samples><Max_Instances>4</Max_Instances></SFXEvent>
  <SFXEvent Name="Gun_Fire"><Samples>GUN.WAV</Samples><Max_Instances>8</Max_Instances><Priority>4</Priority></SFXEvent>
  <SFXEvent Name="Impact_Boom"><Samples>BOOM.WAV</Samples><Max_Instances>8</Max_Instances></SFXEvent>
  <SFXEvent Name="Death_Boom"><Samples>DEATH.WAV</Samples><Max_Instances>8</Max_Instances></SFXEvent>

  <SFXEvent Name="HUD_Base_Attack"><Use_Preset>Preset_HUD</Use_Preset><Samples>BA_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="HUD_Rain_Begin"><Use_Preset>Preset_HUD</Use_Preset><Samples>RB_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="HUD_Enemy_Spotted"><Use_Preset>Preset_HUD</Use_Preset><Samples>ES_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="HUD_Spot_Vader"><Use_Preset>Preset_HUD</Use_Preset><Samples>SV_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="HUD_Pending"><Use_Preset>Preset_HUD</Use_Preset><Samples>PE_ENG.WAV</Samples></SFXEvent>
  <SFXEvent Name="GUI_Click"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_GUI>Yes</Is_GUI><Samples>CLICK.WAV</Samples><Max_Instances>4</Max_Instances></SFXEvent>
  <SFXEvent Name="GUI_Bad_Sound"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_GUI>Yes</Is_GUI><Samples>BAD.WAV</Samples></SFXEvent>
  <SFXEvent Name="CB_Move"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Samples>CBM.WAV</Samples></SFXEvent>
  <SFXEvent Name="CB_Attack_Move"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Samples>CBA.WAV</Samples></SFXEvent>
</SFXEvents>
)";

const char* kMusic = R"(<?xml version="1.0"?>
<MusicEvents>
  <MusicEvent Name="Gal_Rebel"><Files>G1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Amb_Temperate"><Files>AT1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Amb_Temperate_2"><Files>AT2.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Bat_Temperate"><Files>BT1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Amb_Space"><Files>AS1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Bat_Space"><Files>BS1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Bat_Space_DS"><Files>BDS.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Pending"><Files>PEND.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="LoadScreen"><Files>LOAD.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Win_Vs_Empire"><Files>WVE.MP3</Files></MusicEvent>
  <MusicEvent Name="Win"><Files>WIN.MP3</Files></MusicEvent>
  <MusicEvent Name="Lose"><Files>LOSE.MP3</Files></MusicEvent>
  <MusicEvent Name="Summary_Win"><Files>SW.MP3</Files></MusicEvent>
  <MusicEvent Name="Main_Menu_Music_Event"><Files>MENU.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Splash_E3_Music_Event"><Files>SPLASH.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="TSW_Start"><Files>TSW.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Build_Done"><Files>BUILD.MP3</Files></MusicEvent>
  <MusicEvent Name="Movie_Music"><Files>MOV1.MP3, MOV2.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Movie_Cue"><Files>CUE.MP3</Files></MusicEvent>
</MusicEvents>
)";

const char* kSpeech = R"(<?xml version="1.0"?>
<SpeechEvents>
  <SpeechEvent Name="Brief_1"><Files>B1_ENG.MP3</Files><Text_ID>TEXT_B1</Text_ID></SpeechEvent>
  <SpeechEvent Name="Brief_2"><Files>B2_ENG.MP3</Files></SpeechEvent>
  <SpeechEvent Name="Brief_3"><Files>B3_ENG.MP3</Files></SpeechEvent>
</SpeechEvents>
)";

const char* kAudio = R"(<?xml version="1.0"?>
<Audio>
  <Audio_Land_3D_Listener_Z_Pullback_Dist> 60.0 </Audio_Land_3D_Listener_Z_Pullback_Dist>
  <Ambient_Map_Sounds> Temperate, Ambient_Map_Sounds_Temperate </Ambient_Map_Sounds>
  <SFXEvent_GUI_Negative_Feedback> GUI_Bad_Sound </SFXEvent_GUI_Negative_Feedback>
  <SFXEvent_Command_Bar_Move> CB_Move </SFXEvent_Command_Bar_Move>
  <SFXEvent_Command_Bar_Attack_Move> CB_Attack_Move </SFXEvent_Command_Bar_Attack_Move>
  <Music_Land_Battle_To_Ambient_Peace_Seconds> 15.0 </Music_Land_Battle_To_Ambient_Peace_Seconds>
  <Music_Space_Battle_To_Ambient_Peace_Seconds> 10.0 </Music_Space_Battle_To_Ambient_Peace_Seconds>
  <Delay_Between_Land_Base_Attack_Announcement_Seconds> 10 </Delay_Between_Land_Base_Attack_Announcement_Seconds>
  <Music_Event_Battle_End_Summary_Screen_Win> Summary_Win </Music_Event_Battle_End_Summary_Screen_Win>
</Audio>
)";

const char* kWeather = R"(<?xml version="1.0" ?>
<WeatherAudio>
  <Weather_SFXEvent_Loop> CLEAR, 0.00, None </Weather_SFXEvent_Loop>
  <Weather_SFXEvent_Intermittent> CLEAR, 0.00, None, 0.0, 0.0 </Weather_SFXEvent_Intermittent>
  <Weather_SFXEvent_Loop> RAIN, 0.75, Rain_Heavy_Loop </Weather_SFXEvent_Loop>
  <Weather_SFXEvent_Loop> RAIN, 0.25, Rain_Light_Loop </Weather_SFXEvent_Loop>
  <Weather_SFXEvent_Loop> RAIN, 0.00, None </Weather_SFXEvent_Loop>
  <Weather_SFXEvent_Intermittent> RAIN, 0.66, Thunder, 8.0, 12.0 </Weather_SFXEvent_Intermittent>
  <Weather_SFXEvent_Intermittent> RAIN, 0.00, None, 0.0, 0.0 </Weather_SFXEvent_Intermittent>
  <Weather_SFXEvent_Loop> BOGUS </Weather_SFXEvent_Loop>
  <Ambient_SFXEvent_Intermittent> TEMPERATE, Birds, 4.0, 8.0 </Ambient_SFXEvent_Intermittent>
</WeatherAudio>
)";

const char* kAmbientMap = R"(<?xml version="1.0" ?>
<AmbientMapSounds>
  <AmbientMapSound Name="Ambient_Map_Sounds_Temperate">
    <Ambient> Bed_Cricket, 30, 100, 1, 2 </Ambient>
  </AmbientMapSound>
  <Ambient_Map_Sound Name="Arctic"></Ambient_Map_Sound>
</AmbientMapSounds>
)";

const char* kFactions = R"(<?xml version="1.0" ?>
<Factions>
  <Faction Name="Rebel">
    <Strategic_Map_Music_Event> Gal_Rebel </Strategic_Map_Music_Event>
    <Music_Event_Battle_Load_Screen> LoadScreen </Music_Event_Battle_Load_Screen>
    <Music_Event_Tactical_Win_Vs_Faction> Empire, Win_Vs_Empire </Music_Event_Tactical_Win_Vs_Faction>
    <Music_Event_Tactical_Win> Win </Music_Event_Tactical_Win>
    <Music_Event_Tactical_Lose> Lose </Music_Event_Tactical_Lose>
    <Music_Event_Space_Battle_Super_Weapon> Bat_Space_DS </Music_Event_Space_Battle_Super_Weapon>
    <Music_Event_Space_Ambient_Super_Weapon> </Music_Event_Space_Ambient_Super_Weapon>
    <Faction_Super_Weapon> Death_Star </Faction_Super_Weapon>
    <SFX_Event_Tactical_Land_Battle_Pending> HUD_Pending </SFX_Event_Tactical_Land_Battle_Pending>
    <Music_Event_Tactical_Land_Battle_Pending> Pending </Music_Event_Tactical_Land_Battle_Pending>
    <Music_Event_List_Ambient> Space, Amb_Space </Music_Event_List_Ambient>
    <Music_Event_List_Ambient> Temperate, Amb_Temperate </Music_Event_List_Ambient>
    <Music_Event_List_Ambient> Temperate, Amb_Temperate_2 </Music_Event_List_Ambient>
    <Music_Event_List_Battle> Space, Bat_Space </Music_Event_List_Battle>
    <Music_Event_List_Battle> Temperate, Bat_Temperate </Music_Event_List_Battle>
    <SFXEvent_Land_Base_Under_Attack_Announcement> HUD_Base_Attack </SFXEvent_Land_Base_Under_Attack_Announcement>
    <SFXEvent_Unit_Type_Spotted> Darth_Vader, HUD_Spot_Vader </SFXEvent_Unit_Type_Spotted>
    <SFXEvent_Enemy_Spotted> HUD_Enemy_Spotted </SFXEvent_Enemy_Spotted>
    <SFXEvent_Weather_Begin> RAIN, HUD_Rain_Begin </SFXEvent_Weather_Begin>
    <SFXEvent_Weather_End> RAIN, </SFXEvent_Weather_End>
    <Credits> 100 </Credits>
  </Faction>
  <Faction Name="Rebel"><Strategic_Map_Music_Event> Other </Strategic_Map_Music_Event></Faction>
</Factions>
)";

MemoryProvider make_tree() {
    MemoryProvider p;
    p.add("Data\\XML\\SFXEventFiles.xml", "<SFXEvent_Files><File>SFXEventsTest.xml</File></SFXEvent_Files>");
    p.add("Data\\XML\\SFXEventsTest.xml", kSfx);
    p.add("Data\\XML\\MusicEvents.xml", kMusic);
    p.add("Data\\XML\\SpeechEvents.xml", kSpeech);
    p.add("Data\\XML\\Audio.xml", kAudio);
    p.add("Data\\XML\\WeatherAudio.xml", kWeather);
    p.add("Data\\XML\\AmbientMapSounds.xml", kAmbientMap);
    p.add("Data\\XML\\FactionFiles.xml", "<Faction_Files><File>Factions.xml</File><File>Gone.xml</File></Faction_Files>");
    p.add("Data\\XML\\Factions.xml", kFactions);
    p.add("Data\\XML\\AnimationSFXMaps.txt",
          "SURFACE\tinf_move\t0\tINFANTRY_FOOT_PLANT\tB_Foot_R\r\n"
          "SURFACE\tinf_move\t1032\tINFANTRY_FOOT_PLANT\tB_Foot_L\r\n\r\n"
          "SOUND\twalker_move\t3145\tGun_Fire\r\n"
          "SOUND\twalker_move\t800\tImpact_Boom\r\n"
          "BOGUS line\r\n"
          "SOUND\twalker_move\tabc\tGun_Fire");
    return p;
}

std::optional<double> test_duration(const SampleRef& r) {
    if (r.bus == Bus::Music) return 30.0;
    if (r.bus == Bus::Speech) return 2.0;
    if (iequals(r.file, "RAIN_H.WAV") || iequals(r.file, "RAIN_L.WAV")) return 5.0;
    return 0.5;
}

int count_op(const AudioSystem& a, LogOp op, std::string_view event = {}) {
    int n = 0;
    for (const LogEntry& e : a.log()) n += (e.op == op && (event.empty() || iequals(e.event, event))) ? 1 : 0;
    return n;
}
const LogEntry* last_op(const AudioSystem& a, LogOp op, std::string_view event = {}) {
    for (auto it = a.log().rbegin(); it != a.log().rend(); ++it)
        if (it->op == op && (event.empty() || iequals(it->event, event))) return &*it;
    return nullptr;
}
// Index of the first log entry with op/event at or after `from`; -1 if none.
long find_op(const AudioSystem& a, LogOp op, std::string_view event, size_t from = 0) {
    for (size_t i = from; i < a.log().size(); ++i)
        if (a.log()[i].op == op && iequals(a.log()[i].event, event)) return static_cast<long>(i);
    return -1;
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
float dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Rig {
    MemoryProvider tree = make_tree();
    AudioDatabase db;
    GameAudioData data;
    GameAudioLoadReport report;
    NullBackend backend{test_duration};
    SimRng rng{0xBEEFu};
    std::unique_ptr<AudioSystem> a;
    std::vector<Vec3> positions;  // every 3D-positional start point (gate 9 callback)
    Rig() {
        db.load(tree);
        data.load(tree, &report);
        a = std::make_unique<AudioSystem>(db, backend, rng);
        a->set_position_fogged([this](const Vec3& p) {
            positions.push_back(p);
            return false;
        });
    }
};

// ------------------------------------------------------------------ data

void test_data() {
    Rig rig;
    const GameAudioLoadReport& r = rig.report;
    CHECK(r.weather_loops == 4 && r.weather_intermittents == 3 && r.ambient_intermittents == 1);
    CHECK(r.ambient_map_sounds == 2 && r.factions == 1);
    bool bad_row = false, dup = false, missing = false;
    for (const std::string& d : r.diagnostics) {
        bad_row |= d.find("bad Weather_SFXEvent_Loop") != std::string::npos;
        dup |= d.find("duplicate Faction Rebel") != std::string::npos;
        missing |= d.find("Gone.xml") != std::string::npos;
    }
    CHECK(bad_row && dup && missing);
    const WeatherAudioTable& w = rig.data.weather();
    CHECK(w.loop_for("RAIN", 1.0f)->event == "Rain_Heavy_Loop");
    CHECK(w.loop_for("rain", 0.75f)->event == "Rain_Heavy_Loop");
    CHECK(w.loop_for("RAIN", 0.74f)->event == "Rain_Light_Loop");
    CHECK(w.loop_for("RAIN", 0.25f)->event == "Rain_Light_Loop");
    CHECK(w.loop_for("RAIN", 0.1f)->event.empty());  // None
    CHECK(w.loop_for("CLEAR", 0.0f)->event.empty());
    CHECK(w.loop_for("SNOW", 1.0f) == nullptr);
    CHECK(w.intermittent_for("RAIN", 0.7f)->event == "Thunder" && w.intermittent_for("RAIN", 0.7f)->max_seconds == 12.0f);
    CHECK(w.intermittent_for("RAIN", 0.5f)->event.empty());
    CHECK(w.ambient_for("Temperate")->event == "Birds" && w.ambient_for("Urban") == nullptr);
    const AmbientMapSound* t = rig.data.find_ambient_map_sound("ambient_map_sounds_temperate");
    CHECK(t && t->entries.size() == 1 && t->entries[0].event == "Bed_Cricket" && t->entries[0].max_distance == 100.0f &&
          t->entries[0].max_seconds == 2.0f);
    CHECK(rig.data.find_ambient_map_sound("Arctic") && rig.data.find_ambient_map_sound("Arctic")->entries.empty());
    const FactionAudio* f = rig.data.find_faction("REBEL");
    CHECK(f && f->get("Strategic_Map_Music_Event") == "Gal_Rebel");  // the duplicate faction is not kept
    CHECK(f->get("Credits").empty());                                 // only SFX/Music/Speech tags are kept
    CHECK(f->get("Music_Event_Space_Ambient_Super_Weapon").empty());
    CHECK(f->get("Faction_Super_Weapon") == "Death_Star");  // b0286: kept for the super-weapon music choice
    CHECK((f->pair_values("Music_Event_List_Ambient", "temperate") ==
           std::vector<std::string>{"Amb_Temperate", "Amb_Temperate_2"}));
    CHECK(f->pair_values("SFXEvent_Weather_End", "RAIN").empty());  // "RAIN, " = no event
    // AnimationSFXMaps.txt
    const AnimationSfxMap& am = rig.data.animation_sfx();
    CHECK(r.anim_sfx_entries == 4 && am.animation_count() == 2);
    int bad_lines = 0;
    for (const std::string& d : r.diagnostics) bad_lines += d.find("AnimationSFXMaps.txt") != std::string::npos ? 1 : 0;
    CHECK(bad_lines == 2);
    const auto* wm = am.entries("WALKER_MOVE");
    CHECK(wm && wm->size() == 2 && (*wm)[0].time_ms == 800 && (*wm)[1].name == "Gun_Fire");
    CHECK(am.crossed("walker_move", -1, 1000).size() == 1 && am.crossed("walker_move", 800, 3144).empty());
    const auto first = am.crossed("inf_move", -1, 0);
    CHECK(first.size() == 1 && first[0]->kind == AnimSfxEntry::Kind::Surface && first[0]->bone == "B_Foot_R");
    CHECK(am.crossed("inf_move", 0, 2000).size() == 1 && am.crossed("nothing", -1, 99999).empty());
}

// ------------------------------------------------------------------ ambience

void test_ambience() {
    Rig rig;
    AudioSystem& a = *rig.a;
    AmbienceManager am(a, rig.data, rig.rng);
    auto step = [&](double dt, int n) {
        for (int i = 0; i < n; ++i) {
            a.update(dt);
            am.update(dt);
        }
    };
    am.begin("Temperate");
    CHECK(am.active() && am.bed_count() == 1);
    // Clear weather: no loop, the map intermittent (Birds) runs, the weather intermittent is None.
    step(0.0, 1);
    CHECK(am.loop_event().empty() && am.weather_intermittent_event().empty() && am.map_intermittent_event() == "Birds");
    // 10 s of clear weather: Birds every 4..8 s at 50 units, crickets every 1..2 s at 30..100 units.
    rig.positions.clear();
    step(0.1, 100);
    const int birds = count_op(a, LogOp::SfxStart, "Birds"), crickets = count_op(a, LogOp::SfxStart, "Bed_Cricket");
    CHECK(birds >= 1 && birds <= 3);
    CHECK(crickets >= 5 && crickets <= 11);
    CHECK(static_cast<int>(rig.positions.size()) == birds + crickets);
    int at50 = 0, in_bed_range = 0;
    for (const Vec3& p : rig.positions) {
        const float d = dist(p, a.listener().position);
        at50 += near(d, 50.0f, 1e-3f) ? 1 : 0;
        in_bed_range += (d >= 30.0f - 1e-3f && d <= 100.0f + 1e-3f) ? 1 : 0;
    }
    CHECK(at50 >= birds && in_bed_range == static_cast<int>(rig.positions.size()));
    // Points lie in the listener's horizontal plane (default up = +Y).
    for (const Vec3& p : rig.positions) CHECK(near(p.y, 0.0f));

    // Heavy rain: heavy loop with Loop_Fade_In 3 s, thunder armed 8..12 s, birds off.
    am.set_weather("RAIN", 0.9f);
    step(0.0, 1);
    CHECK(am.loop_event() == "Rain_Heavy_Loop" && am.loop_handle() != 0);
    CHECK(am.weather_intermittent_event() == "Thunder" && am.map_intermittent_event().empty());
    CHECK(am.weather_intermittent_remaining() >= 8.0 && am.weather_intermittent_remaining() <= 12.0);
    const uint32_t heavy = am.loop_handle();
    step(0.0, 1);  // the loop voice starts on the next SFX update, at fade-in level 0
    const LogEntry* v = last_op(a, LogOp::SfxVoice, "Rain_Heavy_Loop");
    CHECK(v && v->gain == 0.0f);
    // Int-final: copy the voice id now; step() appends to the log and may reallocate it, leaving `v` dangling
    // (read of the freed buffer crashed the gcc build).
    const uint32_t heavy_voice = v ? v->voice : 0;
    step(0.1, 15);  // 1.5 s: half way up (55 % volume x SFX bus 0.8 x master 0.8)
    const NullBackend::Voice* bv = rig.backend.voice(heavy_voice);
    CHECK(bv && near(bv->params.gain, 0.55f * 0.64f * 0.5f, 2e-3f) && bv->params.loop);
    const int birds_before = count_op(a, LogOp::SfxStart, "Birds");
    step(0.1, 130);  // 13 s: at least one thunder, no birds
    CHECK(count_op(a, LogOp::SfxStart, "Thunder") >= 1 && count_op(a, LogOp::SfxStart, "Birds") == birds_before);
    CHECK(a.sfx_live(heavy));

    // Lighter rain: the heavy loop fades out over 7 s while the light one fades in (crossfade).
    am.set_weather("RAIN", 0.3f);
    step(0.0, 1);
    CHECK(am.loop_event() == "Rain_Light_Loop" && am.weather_intermittent_event().empty());
    const LogEntry* stop = last_op(a, LogOp::SfxStop, "Rain_Heavy_Loop");
    CHECK(stop && stop->reason == "fade");
    step(0.5, 10);
    CHECK(a.sfx_live(heavy) && a.sfx_live(am.loop_handle()));
    step(0.5, 5);  // 7.5 s
    CHECK(!a.sfx_live(heavy) && a.sfx_live(am.loop_handle()));
    // Rain at 0.1: loop None (silence), the clear-weather map intermittent comes back.
    am.set_weather("RAIN", 0.1f);
    step(0.0, 1);
    CHECK(am.loop_event().empty() && am.map_intermittent_event() == "Birds");

    // Weather_Audio_Pause: the loop fades out, countdowns stop; unpause restores both. Beds keep running.
    am.set_weather("RAIN", 1.0f);
    step(0.1, 2);
    const uint32_t h2 = am.loop_handle();
    am.set_paused(true);
    step(0.1, 1);
    CHECK(am.loop_event().empty() && am.weather_intermittent_event().empty());
    const int thunders = count_op(a, LogOp::SfxStart, "Thunder"), beds = count_op(a, LogOp::SfxStart, "Bed_Cricket");
    step(0.5, 60);  // 30 s paused
    CHECK(!a.sfx_live(h2) && count_op(a, LogOp::SfxStart, "Thunder") == thunders);
    CHECK(count_op(a, LogOp::SfxStart, "Bed_Cricket") > beds);
    am.set_paused(false);
    step(0.1, 1);
    CHECK(am.loop_event() == "Rain_Heavy_Loop" && a.sfx_live(am.loop_handle()) && am.weather_intermittent_event() == "Thunder");

    // A mode change's stop_all kills the loop: it is restarted after retry_seconds (1 s).
    a.stop_all_sfx();
    const uint32_t killed = am.loop_handle();
    step(0.1, 5);
    CHECK(am.loop_handle() == killed && !a.sfx_live(killed));
    step(0.1, 6);
    CHECK(am.loop_handle() != killed && a.sfx_live(am.loop_handle()));

    // end(): the loop fades with its Loop_Fade_Out_Seconds; nothing new starts.
    am.end();
    const size_t log_size = a.log().size();
    step(0.5, 20);
    CHECK(a.live_instances() == 0);
    CHECK(find_op(a, LogOp::SfxStart, "Thunder", log_size) < 0 && find_op(a, LogOp::SfxStart, "Rain_Heavy_Loop", log_size) < 0);

    // Beds off by config; space has no ambience at all.
    AmbienceConfig cfg;
    cfg.map_beds = false;
    AmbienceManager quiet(a, rig.data, rig.rng, cfg);
    quiet.begin("Temperate");
    CHECK(quiet.bed_count() == 0);
    AmbienceManager space(a, rig.data, rig.rng);
    space.begin("Space");
    space.set_weather("CLEAR", 0.0f);
    space.update(0.0);
    CHECK(space.loop_event().empty() && space.map_intermittent_event().empty() && space.bed_count() == 0);
}

// ------------------------------------------------------------------ conversations

void test_conversation() {
    Rig rig;
    AudioSystem& a = *rig.a;
    ConversationPlayer cp(a);
    std::vector<std::pair<uint32_t, bool>> finished;
    cp.on_finished([&](uint32_t id, bool done) { finished.emplace_back(id, done); });
    std::vector<std::string> subtitles;
    a.on_speech_start([&](const SpeechEvent& e, const std::string& file) {
        subtitles.push_back(e.text_ids.empty() ? file : e.text_ids[0]);
    });
    auto step = [&](double dt, int n) {
        for (int i = 0; i < n; ++i) {
            a.update(dt);
            cp.update(dt);
        }
    };
    Conversation c1;
    c1.lines.push_back({ConversationLine::Kind::Speech, "Brief_1", 0, {}, 0.0f});
    c1.lines.push_back({ConversationLine::Kind::Sfx, "GUI_Click", 0, {}, 0.5f});
    c1.lines.push_back({ConversationLine::Kind::Speech, "No_Such_Line", 0, {}, 0.0f});
    c1.lines.push_back({ConversationLine::Kind::Speech, "Brief_2", 0, {}, 0.0f});
    const uint32_t id1 = cp.start(c1);
    step(0.1, 2);
    CHECK(cp.playing() == id1 && a.current_speech() == "Brief_1");
    CHECK(!subtitles.empty() && subtitles[0] == "TEXT_B1");
    // While the line plays: SFX capped at 0.3 (ducking), HUD VO refused (Â§3.2).
    CHECK(a.play_sfx("HUD_Base_Attack") == 0);
    const uint32_t gun = a.play_sfx("Gun_Fire");
    a.update(0.0);
    const LogEntry* gv = last_op(a, LogOp::SfxVoice, "Gun_Fire");
    CHECK(gun != 0 && gv && gv->gain <= 0.3f + 1e-6f);
    step(0.1, 60);  // 6 s: Brief_1 (2 s), click (0.5 s) + 0.5 s pause, unknown line skipped, Brief_2 (2 s)
    const long s1 = find_op(a, LogOp::SpeechEnd, "Brief_1"), click = find_op(a, LogOp::SfxStart, "GUI_Click");
    const long end_click = find_op(a, LogOp::SfxEnd, "GUI_Click"), s2 = find_op(a, LogOp::SpeechStart, "Brief_2");
    CHECK(s1 >= 0 && click > s1 && end_click > click && s2 > end_click);
    CHECK(a.log()[static_cast<size_t>(s2)].time_us - a.log()[static_cast<size_t>(end_click)].time_us >= 500000);
    CHECK(count_op(a, LogOp::SpeechQueue, "No_Such_Line") == 1);
    CHECK(finished.size() == 1 && finished[0] == std::make_pair(id1, true) && !cp.active(id1));

    // Priorities: B waits behind A; C (more important) interrupts A, then B plays.
    finished.clear();
    Conversation ca, cb, cc;
    ca.lines = {{ConversationLine::Kind::Speech, "Brief_1", 0, {}, 0.0f}, {ConversationLine::Kind::Speech, "Brief_2", 0, {}, 0.0f}};
    cb.lines = {{ConversationLine::Kind::Speech, "Brief_3", 0, {}, 0.0f}};
    cc.lines = {{ConversationLine::Kind::Speech, "Brief_2", 0, {}, 0.0f}};
    cc.priority = 1;
    const uint32_t ida = cp.start(ca);
    const uint32_t idb = cp.start(cb);
    step(0.1, 3);
    CHECK(cp.playing() == ida && cp.waiting() == 1 && a.current_speech() == "Brief_1");
    const uint32_t idc = cp.start(cc);
    CHECK(finished.size() == 1 && finished[0] == std::make_pair(ida, false));
    const LogEntry* kill = last_op(a, LogOp::SpeechKill, "Brief_1");
    CHECK(kill && kill->reason == "cancel" && !a.speech_playing());
    step(0.1, 2);
    CHECK(cp.playing() == idc && a.current_speech() == "Brief_2");
    step(0.1, 25);
    CHECK(cp.playing() == idb && a.current_speech() == "Brief_3");
    step(0.1, 25);
    CHECK(finished.size() == 3 && finished[1] == std::make_pair(idc, true) && finished[2] == std::make_pair(idb, true));
    // Cancel a waiting and the playing conversation.
    const uint32_t x = cp.start(cb), y = cp.start(cb);
    step(0.1, 2);
    cp.cancel(y);
    cp.cancel(x);
    CHECK(!cp.active(x) && !cp.active(y) && finished.size() == 5 && !a.speech_playing());
    // Object-attached SFX line.
    Conversation co;
    co.lines = {{ConversationLine::Kind::Sfx, "Unit_Select", 77, {1.0f, 2.0f, 3.0f}, 0.0f}};
    cp.start(co);
    step(0.1, 2);
    const LogEntry* us = last_op(a, LogOp::SfxStart, "Unit_Select");
    CHECK(us && us->owner == 77);
}

// ------------------------------------------------------------------ GameAudio hooks

void test_music_modes() {
    Rig rig;
    AudioSystem& a = *rig.a;
    GameAudio g(a, rig.data, rig.rng);
    g.set_local_faction("Rebel");
    g.enter_front_end();
    CHECK(a.current_music() == "Main_Menu_Music_Event" && a.music_mode() == music_mode::kFrontEnd && a.music_mode() == 13);
    g.enter_galactic();
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic && a.mode() == AudioMode::Galactic);
    g.battle_pending(true);
    CHECK(a.current_music() == "Pending" && a.music_mode() == music_mode::kBattlePending);
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Pending") == 1);
    g.battle_load_screen(true);
    CHECK(a.current_music() == "LoadScreen" && a.music_mode() == music_mode::kLoadScreen && a.music_mode() == 15);
    // 3D SFX are silent behind the load screen.
    const uint32_t boom = g.impact("Impact_Boom", {10.0f, 0.0f, 0.0f});
    g.update(0.0);
    const LogEntry* bv = last_op(a, LogOp::SfxVoice, "Impact_Boom");
    CHECK(boom != 0 && bv && bv->gain == 0.0f);
    g.battle_load_screen(false);
    TacticalSetup land;
    land.environment = "Temperate";
    g.enter_tactical(land);
    CHECK(a.mode() == AudioMode::Land && a.music_mode() == music_mode::kTacticalAmbient);
    CHECK(a.current_music() == "Amb_Temperate" || a.current_music() == "Amb_Temperate_2");
    CHECK(g.ambience().active() && g.ambience().bed_count() == 1);
    // Combat -> battle music; 15 s of sim peace -> ambient again (peace is sim ticks, Â§2.3).
    g.combat({false, false});
    CHECK(a.music_mode() == music_mode::kTacticalAmbient);
    g.combat({true, false});
    CHECK(a.current_music() == "Bat_Temperate" && a.music_mode() == music_mode::kTacticalBattle);
    for (int t = 0; t < 449; ++t) g.sim_tick();
    CHECK(a.music_mode() == music_mode::kTacticalBattle);
    g.combat({true, false});  // resets the counter
    for (int t = 0; t < 449; ++t) g.sim_tick();
    CHECK(a.music_mode() == music_mode::kTacticalBattle);
    g.sim_tick();
    CHECK(a.music_mode() == music_mode::kTacticalAmbient);
    // Victory against the Empire: the _Vs_Faction row; combat no longer changes the music.
    g.battle_end({true, "Empire", false});
    CHECK(a.current_music() == "Win_Vs_Empire" && a.music_mode() == music_mode::kBattleEnd && a.music_mode() == 16);
    g.combat({true, false});
    CHECK(a.current_music() == "Win_Vs_Empire");
    g.battle_end({false, "Hutts", false});
    CHECK(a.current_music() == "Lose");
    g.battle_summary_screen(true);
    CHECK(a.current_music() == "Summary_Win");
    CHECK(a.music_mode() == music_mode::kBattleEnd);
    // Battle end (16) is not in the excluded set of automatic mode music (8, 11, 12): an unforced
    // Resume_Mode_Based_Music picks the mode music again.
    a.resume_mode_based_music(false);
    CHECK(a.music_mode() == music_mode::kTacticalAmbient &&
          (a.current_music() == "Amb_Temperate" || a.current_music() == "Amb_Temperate_2"));
    // Space with a super weapon: the ambient super-weapon event is empty -> list; battle -> super-weapon event.
    TacticalSetup space;
    space.land = false;
    space.super_weapon_present = true;
    g.enter_tactical(space);
    CHECK(a.mode() == AudioMode::Space && a.current_music() == "Amb_Space");
    g.combat({true, false});
    CHECK(a.current_music() == "Bat_Space_DS");
    space.super_weapon_present = false;
    g.enter_tactical(space);
    g.combat({true, false});
    CHECK(a.current_music() == "Bat_Space");
    // b0286: the host's first battle report (presence learnt one tick after enter_tactical) re-picks the mode's event;
    // later reports only change the flag, read at the next mode switch.
    g.set_super_weapon_present(true, true);
    CHECK(g.super_weapon_present() && a.current_music() == "Bat_Space_DS" && a.music_mode() == music_mode::kTacticalBattle);
    g.set_super_weapon_present(false, false);
    CHECK(!g.super_weapon_present() && a.current_music() == "Bat_Space_DS");
    g.set_super_weapon_present(false, true);  // no change: nothing restarts
    CHECK(a.current_music() == "Bat_Space_DS");
    // An unknown faction resolves nothing (music unchanged).
    g.set_local_faction("Nobody");
    g.enter_galactic();
    CHECK(a.current_music() == "Bat_Space_DS");
}

// STRATEGIC_GAPS §7.4: modes 4, 8, 11, 13 (lock), 14, 17.
void test_music_modes_gaps() {
    Rig rig;
    AudioSystem& a = *rig.a;
    GameAudio g(a, rig.data, rig.rng);
    g.set_local_faction("Rebel");
    auto run = [&](double seconds) {
        for (int i = 0; i < static_cast<int>(seconds); ++i) g.update(1.0);
    };
    // Splash (11): excluded from unforced automatic mode music.
    g.splash_screen();
    CHECK(a.current_music() == "Splash_E3_Music_Event" && a.music_mode() == music_mode::kSplash && a.music_mode() == 11);
    a.resume_mode_based_music(false);
    CHECK(a.current_music() == "Splash_E3_Music_Event");
    // Front end (13) with the attract lock set: refused unless forced; cleared lock -> plays.
    a.set_music_lock(true);
    g.enter_front_end();
    CHECK(a.current_music() == "Splash_E3_Music_Event");
    const LogEntry* lr = last_op(a, LogOp::MusicRefuse, "Main_Menu_Music_Event");
    CHECK(lr && lr->reason == "locked");
    CHECK(a.play_music("Main_Menu_Music_Event", music_mode::kFrontEnd, false, true));  // forced
    CHECK(a.current_music() == "Main_Menu_Music_Event");
    // The lock refuses only mode-13 requests: other modes still play while it is set.
    g.enter_galactic();
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic);
    a.set_music_lock(false);
    g.enter_front_end();
    CHECK(a.current_music() == "Main_Menu_Music_Event" && a.music_mode() == music_mode::kFrontEnd);
    // Production complete (4): plays once, then the previous event resumes in its mode.
    g.enter_galactic();
    run(1.0);
    g.build_complete("Build_Done");
    CHECK(a.current_music() == "Build_Done" && a.music_mode() == music_mode::kBuildComplete && a.music_mode() == 4);
    run(35.0);
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic);
    // Movie music (14): a looping playlist stops after one pass (2 files) and resumes the previous event.
    g.movie_music("Movie_Music");
    CHECK(a.current_music() == "Movie_Music" && a.music_mode() == music_mode::kMovie && a.music_mode() == 14);
    run(35.0);
    CHECK(a.current_music() == "Movie_Music");
    run(35.0);
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic);
    // Movie cue (17).
    g.movie_music_cue("Movie_Cue");
    CHECK(a.current_music() == "Movie_Cue" && a.music_mode() == music_mode::kMovieCue && a.music_mode() == 17);
    // [port] The movie ends early (skipped): its music stops and the music it replaced restarts in its own mode.
    CHECK(g.movie_music_end());
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic);
    CHECK(!g.movie_music_end());  // nothing of a movie playing: no change
    CHECK(a.current_music() == "Gal_Rebel");
    g.movie_music("Movie_Music");
    run(2.0);
    CHECK(g.movie_music_end());
    CHECK(a.current_music() == "Gal_Rebel" && a.music_mode() == music_mode::kGalactic);
    CHECK(!a.end_music_mode(music_mode::kMovie));  // wrong mode: untouched
    // Halt_Game_Music: every music stream stops.
    g.halt_game_music();
    CHECK(a.current_music().empty());
    a.resume_mode_based_music(false);
    CHECK(a.current_music() == "Gal_Rebel");
    g.movie_music_cue("Movie_Cue");
    // Super weapon activation (8): combat / peace switches and unforced resumes leave it; the next mode entry replaces it.
    TacticalSetup land;
    land.environment = "Temperate";
    g.enter_tactical(land);
    g.super_weapon_activation("TSW_Start");
    CHECK(a.current_music() == "TSW_Start" && a.music_mode() == music_mode::kSuperWeapon && a.music_mode() == 8);
    a.resume_mode_based_music(false);
    CHECK(a.current_music() == "TSW_Start");
    for (int t = 0; t < 500; ++t) g.sim_tick();
    CHECK(a.current_music() == "TSW_Start");
    g.enter_tactical(land);
    CHECK(a.music_mode() == music_mode::kTacticalAmbient);
}

void test_hooks() {
    Rig rig;
    AudioSystem& a = *rig.a;
    GameAudio g(a, rig.data, rig.rng);
    g.set_local_faction("Rebel");
    TacticalSetup land;
    land.environment = "Temperate";
    g.enter_tactical(land);

    // Listener from the camera (Â§1.8): look-at - forward x 60 (land pullback); cinematic = camera position.
    CameraState cam;
    cam.position = {0.0f, 100.0f, -100.0f};
    cam.look_at = {10.0f, 0.0f, 20.0f};
    cam.forward = {0.0f, 0.0f, 2.0f};
    g.set_camera(cam);
    CHECK(near(a.listener().position.z, -40.0f) && near(a.listener().position.x, 10.0f) && !a.cinematic());
    cam.cinematic = true;
    g.set_camera(cam);
    CHECK(near(a.listener().position.y, 100.0f) && a.cinematic());
    cam.cinematic = false;
    g.set_camera(cam);

    // Unit responses (Â§3.3).
    data::XmlDocument doc = data::XmlDocument::parse(std::string_view(
        "<SpaceUnit Name='X'><SFXEvent_Select>Unit_Select</SFXEvent_Select><SFXEvent_Move> Unit_Move </SFXEvent_Move>"
        "<SFXEvent_Group_Move>Unit_Group_Move</SFXEvent_Group_Move>"
        "<SFXEvent_Attack_With_Non_Hero_Ability>Unit_Attack_Ability</SFXEvent_Attack_With_Non_Hero_Ability>"
        "<SFXEvent_Engine_Idle_Loop>Engine_Idle</SFXEvent_Engine_Idle_Loop>"
        "<SFXEvent_Engine_Moving_Loop>Engine_Move</SFXEvent_Engine_Moving_Loop>"
        "<SFXEvent_Ambient_Loop>Ambient_Hum</SFXEvent_Ambient_Loop><SFXEvent_Ambient_Moving>Chatter_Moving</SFXEvent_Ambient_Moving>"
        "<SFXEvent_Ambient_Moving_Min_Delay_Seconds>2</SFXEvent_Ambient_Moving_Min_Delay_Seconds>"
        "<SFXEvent_Ambient_Moving_Max_Delay_Seconds>3</SFXEvent_Ambient_Moving_Max_Delay_Seconds>"
        "<Death_SFXEvent_Start_Die>Death_Boom</Death_SFXEvent_Start_Die></SpaceUnit>"));
    const UnitSounds us = unit_sounds_from_xml(doc.root());
    CHECK(us.response[static_cast<size_t>(UnitOrder::Move)] == "Unit_Move" && us.ambient_moving_max_seconds == 3.0f);
    data::XmlDocument variant = data::XmlDocument::parse(std::string_view("<SpaceUnit><SFXEvent_Move>Other</SFXEvent_Move></SpaceUnit>"));
    const UnitSounds vs = unit_sounds_from_xml(variant.root(), &us);
    CHECK(vs.response[static_cast<size_t>(UnitOrder::Move)] == "Other" && vs.engine_idle_loop == "Engine_Idle");

    const Vec3 p{5.0f, 0.0f, 5.0f};
    uint32_t h = g.unit_response(9, us, UnitOrder::Select, p);
    CHECK(h != 0 && last_op(a, LogOp::SfxStart)->event == "Unit_Select" && last_op(a, LogOp::SfxStart)->owner == 9);
    CHECK(g.unit_response(9, us, UnitOrder::Select, p) == 0);  // Overlap_Test "U": one line at a time
    g.update(1.0);
    g.update(0.0);
    CHECK(g.unit_response(9, us, UnitOrder::Move, p, true) != 0 && last_op(a, LogOp::SfxStart)->event == "Unit_Group_Move");
    CHECK(g.unit_response(9, us, UnitOrder::FleetMove, p) != 0 && last_op(a, LogOp::SfxStart)->event == "Unit_Move");
    CHECK(g.unit_response(9, us, UnitOrder::Attack, p, true, true) != 0 &&
          last_op(a, LogOp::SfxStart)->event == "Unit_Attack_Ability");
    CHECK(g.unit_response(9, us, UnitOrder::Guard, p) == 0);  // no line for this type
    a.allow_unit_response_vo(false);
    CHECK(g.unit_response(9, us, UnitOrder::Move, p) == 0);
    a.allow_unit_response_vo(true);

    // Objects: ambient + engine idle loops at spawn, idle -> moving crossfade, moving chatter, death.
    g.object_spawned(50, us, p);
    g.update(0.0);
    CHECK(count_op(a, LogOp::SfxStart, "Ambient_Hum") == 1 && count_op(a, LogOp::SfxStart, "Engine_Idle") == 1);
    g.object_moving(50, true);
    const LogEntry* idle_stop = last_op(a, LogOp::SfxStop, "Engine_Idle");
    CHECK(idle_stop && idle_stop->reason == "fade" && count_op(a, LogOp::SfxStart, "Engine_Move") == 1);
    g.object_moving(50, true);  // no change
    CHECK(count_op(a, LogOp::SfxStart, "Engine_Move") == 1);
    for (int i = 0; i < 70; ++i) g.update(0.1);  // 7 s moving: chatter every 2..3 s
    const int chatter = count_op(a, LogOp::SfxStart, "Chatter_Moving");
    CHECK(chatter >= 2 && chatter <= 3);
    g.object_moving(50, false);
    CHECK(count_op(a, LogOp::SfxStart, "Engine_Idle") == 2);
    for (int i = 0; i < 40; ++i) g.update(0.1);
    CHECK(count_op(a, LogOp::SfxStart, "Chatter_Moving") == chatter);
    const size_t before_death = a.log().size();
    CHECK(g.unit_death(50, us.death_start, p) != 0);
    g.update(0.0);
    for (size_t i = before_death; i < a.log().size(); ++i) {
        const LogEntry& e = a.log()[i];
        if (e.op == LogOp::SfxVoice) CHECK(e.event == "Death_Boom");
    }
    CHECK(find_op(a, LogOp::SfxStop, "Ambient_Hum", before_death) >= 0);
    g.object_moving(50, true);  // forgotten object: nothing
    CHECK(count_op(a, LogOp::SfxStart, "Engine_Move") == 1);

    // Battle events.
    CHECK(g.weapon_fire(60, "Gun_Fire", p) != 0 && last_op(a, LogOp::SfxStart)->owner == 60);
    CHECK(g.weapon_fire(60, "", p) == 0);
    CHECK(g.explosion("Impact_Boom", p) != 0 && rig.positions.back().x == 5.0f);
    CHECK(g.object_sfx(61, "Ambient_Hum", p, 0.5f) != 0);
    g.stop_object_sfx(61, "Ambient_Hum");
    CHECK(last_op(a, LogOp::SfxStop, "Ambient_Hum")->owner == 61);
    // Animation SOUND rows crossed this frame play attached; SURFACE rows are not audio.
    CHECK(g.animation_sounds(70, "walker_move", 1000, 4000, p) == 1 && last_op(a, LogOp::SfxStart)->owner == 70 &&
          last_op(a, LogOp::SfxStart)->event == "Gun_Fire");
    CHECK(g.animation_sounds(70, "inf_move", -1, 2000, p) == 0);

    // Base-under-attack announcement: rate-limited to one per 10 s x 30 sim ticks.
    g.update(5.0);  // let earlier HUD lines end
    g.combat({true, true});
    g.combat({true, true});
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Base_Attack") == 1);
    for (int t = 0; t < 299; ++t) g.sim_tick();
    g.combat({true, true});
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Base_Attack") == 1);
    g.sim_tick();
    // The first announcement is still playing (one update starts a voice, the next sees it end): the
    // event's own Max_Instances 1 refuses a second one.
    g.combat({true, true});
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Base_Attack") == 1);
    for (int t = 0; t < 300; ++t) g.sim_tick();
    auto settle = [&] {
        g.update(2.0);
        g.update(2.0);
    };
    settle();
    g.combat({true, true});
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Base_Attack") == 2);

    // Weather announcements: begin on CLEAR -> RAIN (> 0); intensity changes do not repeat it.
    settle();
    g.set_weather("RAIN", 0.5f);
    g.set_weather("RAIN", 0.9f);
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Rain_Begin") == 1);
    g.set_weather("CLEAR", 0.0f);
    settle();
    g.set_weather("RAIN", 0.2f);
    CHECK(count_op(a, LogOp::SfxStart, "HUD_Rain_Begin") == 2);

    // Enemy spotted: per-type line, else the generic one; Lua flag off -> nothing.
    settle();
    CHECK(g.enemy_spotted("darth_vader") != 0 && last_op(a, LogOp::SfxStart)->event == "HUD_Spot_Vader");
    settle();
    CHECK(g.enemy_spotted("Boba_Fett") != 0 && last_op(a, LogOp::SfxStart)->event == "HUD_Enemy_Spotted");
    g.allow_enemy_sighted_vo(false);
    settle();
    CHECK(g.enemy_spotted("Boba_Fett") == 0);

    // GUI: pumped once, so the click's voice exists before the next update.
    const size_t before_click = a.log().size();
    CHECK(g.gui_sfx("GUI_Click") != 0 && find_op(a, LogOp::SfxVoice, "GUI_Click", before_click) >= 0);
    CHECK(g.command_bar_sfx(UnitOrder::Move) != 0 && last_op(a, LogOp::SfxStart)->event == "CB_Move");
    CHECK(g.command_bar_sfx(UnitOrder::Attack, true) != 0 && last_op(a, LogOp::SfxStart)->event == "CB_Attack_Move");
    CHECK(g.command_bar_sfx(UnitOrder::Guard) == 0);  // no constant in this Audio.xml
    g.negative_feedback();
    CHECK(last_op(a, LogOp::SfxStart)->event == "GUI_Bad_Sound");
    CHECK(g.faction_sfx("SFXEvent_Nothing") == 0);

    // Speech + leave_mode: every SFX instance, speech, conversation and the ambience stop.
    CHECK(g.speech("Brief_1") != 0);
    Conversation c;
    c.lines = {{ConversationLine::Kind::Speech, "Brief_2", 0, {}, 0.0f}};
    const uint32_t conv = g.conversation(c);
    g.set_weather("RAIN", 1.0f);
    g.update(0.1);
    g.update(0.1);
    CHECK(a.speech_playing() && a.live_instances() > 0 && g.ambience().loop_event() == "Rain_Heavy_Loop");
    g.leave_mode();
    CHECK(a.live_instances() == 0 && !a.speech_playing() && a.speech_queue_size() == 0);
    CHECK(!g.conversations().active(conv) && !g.ambience().active() && !g.in_tactical());

    // Master volume save / restore (Lua Master_Volume_Restore).
    a.set_bus_level(AudioBus::Master, 0.6f);
    a.save_master_volume();
    a.set_bus_level(AudioBus::Master, 0.1f);
    g.master_volume_restore();
    CHECK(near(a.bus_level(AudioBus::Master), 0.6f));
}

// STATION_HANGARS_SOUND.md §3: engine idle / moving loop slots, crossfade restarts, death / removal kills,
// suppression, engines offline, fog mute, Max_Instances 2 voice stealing by camera distance.
uint32_t voice_for(const AudioSystem& a, uint32_t handle) {
    for (auto it = a.log().rbegin(); it != a.log().rend(); ++it)
        if (it->op == LogOp::SfxVoice && it->handle == handle) return it->voice;
    return 0;
}
float voice_gain(const Rig& rig, const AudioSystem& a, uint32_t handle) {
    const NullBackend::Voice* v = rig.backend.voice(voice_for(a, handle));
    return v && v->active ? v->params.gain : -1.0f;
}
int count_owner(const AudioSystem& a, LogOp op, std::string_view event, uint64_t owner, std::string_view reason = {}) {
    int n = 0;
    for (const LogEntry& e : a.log())
        n += (e.op == op && (event.empty() || iequals(e.event, event)) && e.owner == owner &&
              (reason.empty() || e.reason == reason))
                 ? 1
                 : 0;
    return n;
}
uint32_t start_handle(const AudioSystem& a, std::string_view event, uint64_t owner) {
    for (auto it = a.log().rbegin(); it != a.log().rend(); ++it)
        if (it->op == LogOp::SfxStart && iequals(it->event, event) && it->owner == owner) return it->handle;
    return 0;
}

void test_engine_loops() {
    Rig rig;
    AudioSystem& a = *rig.a;
    GameAudio g(a, rig.data, rig.rng);
    TacticalSetup space;
    space.land = false;
    g.enter_tactical(space);
    std::map<uint64_t, AudioSystem::ObjectAudio> objs;
    g.set_object_query([&objs](uint64_t id) -> std::optional<AudioSystem::ObjectAudio> {
        const auto it = objs.find(id);
        if (it == objs.end()) return std::nullopt;
        return it->second;
    });
    UnitSounds us;
    us.engine_idle_loop = "EGL_Idle";
    us.engine_moving_loop = "EGL_Move";
    us.engine_cinematic_focus_loop = "EGL_Focus";

    // Vanilla preset values reach the events (SFXEVENTSENGINES.XML: fade in 0.5 s, out 1.0 s, Max_Instances 2).
    for (const char* n : {"EGL_Idle", "EGL_Move"}) {
        const SfxEvent& e = a.database().sfx()[a.database().find_sfx(n)];
        CHECK(e.loop_fade_in_seconds == 0.5f && e.loop_fade_out_seconds == 1.0f && e.max_instances == 2 && e.play_count == -1);
    }

    // Mode enter (spawn): idle on with a 0.5 s fade-in, moving off.
    objs[100] = {{100.0f, 0.0f, 0.0f}, true};
    g.object_spawned(100, us, objs[100].position);
    auto st = g.engine_loop_state(100);
    CHECK(st && st->idle && !st->moving && !st->dead && !st->suppressed);
    const uint32_t idle1 = start_handle(a, "EGL_Idle", 100);
    CHECK(idle1 != 0 && count_op(a, LogOp::SfxStart, "EGL_Move") == 0);
    g.update(0.0);
    CHECK(voice_gain(rig, a, idle1) == 0.0f);
    g.update(0.25);
    const float half = voice_gain(rig, a, idle1);
    g.update(0.25);
    const float full = voice_gain(rig, a, idle1);
    CHECK(full > 0.0f && near(half, full * 0.5f));
    g.engine_mode_enter(100);  // flags already set: nothing
    g.object_moving(100, false);
    CHECK(count_op(a, LogOp::SfxStart, "EGL_Idle") == 1 && count_op(a, LogOp::SfxStop, "EGL_Idle") == 0);

    // Starts moving: moving on (fade-in) first, then idle off (1.0 s fade-out), same service.
    const size_t mark = a.log().size();
    g.object_moving(100, true);
    const long move_start = find_op(a, LogOp::SfxStart, "EGL_Move", mark);
    const long idle_stop = find_op(a, LogOp::SfxStop, "EGL_Idle", mark);
    CHECK(move_start >= 0 && idle_stop > move_start && a.log()[static_cast<size_t>(idle_stop)].reason == "fade");
    st = g.engine_loop_state(100);
    CHECK(st && !st->idle && st->moving);
    const uint32_t move1 = start_handle(a, "EGL_Move", 100);
    g.update(0.0);
    g.update(0.3);
    CHECK(a.sfx_live(idle1) && near(voice_gain(rig, a, idle1), full * 0.7f, 1e-3f));

    // Slows down 0.3 s later, while the old idle still fades out: the restart is NOT refused. A second idle
    // instance fades in while the first fades out (two instances of one event on one ship).
    g.object_moving(100, false);
    CHECK(count_owner(a, LogOp::SfxReject, "EGL_Idle", 100) == 0);
    const uint32_t idle2 = start_handle(a, "EGL_Idle", 100);
    CHECK(idle2 != 0 && idle2 != idle1 && a.sfx_live(idle1) && a.sfx_live(idle2) && a.sfx_live(move1));
    g.update(0.0);
    CHECK(voice_gain(rig, a, idle2) == 0.0f && voice_gain(rig, a, idle1) > 0.0f);
    g.update(0.75);  // old idle: 0.7 - 0.75 -> removed; new idle fully faded in; moving still fading (1.0 s)
    CHECK(!a.sfx_live(idle1) && a.sfx_live(idle2) && a.sfx_live(move1) && near(voice_gain(rig, a, idle2), full));
    // Moving again during the moving loop's own fade-out: a second moving instance, also accepted.
    g.object_moving(100, true);
    const uint32_t move2 = start_handle(a, "EGL_Move", 100);
    CHECK(move2 != move1 && a.sfx_live(move1) && a.sfx_live(move2) && count_owner(a, LogOp::SfxReject, "EGL_Move", 100) == 0);
    g.object_moving(100, false);
    const uint32_t idle3 = start_handle(a, "EGL_Idle", 100);
    for (int i = 0; i < 12; ++i) g.update(0.1);
    CHECK(!a.sfx_live(move1) && !a.sfx_live(move2) && !a.sfx_live(idle2) && a.sfx_live(idle3) && idle3 != idle2);

    // Max_Instances 2: three idle ships, only the two nearest to the listener hold a voice.
    objs[101] = {{50.0f, 0.0f, 0.0f}, true};
    objs[102] = {{5000.0f, 0.0f, 0.0f}, true};
    g.object_spawned(101, us, objs[101].position);
    g.object_spawned(102, us, objs[102].position);
    g.update(0.0);
    const uint32_t i101 = start_handle(a, "EGL_Idle", 101), i102 = start_handle(a, "EGL_Idle", 102);
    CHECK(voice_for(a, idle3) != 0 && voice_for(a, i101) != 0 && voice_for(a, i102) == 0);
    CHECK(count_owner(a, LogOp::SfxNoVoice, "EGL_Idle", 102) >= 1 && a.sfx_live(i102));
    // Camera moves next to ship 102: it retries, takes the farthest holder's voice (101); no start/stop calls.
    a.set_listener(Listener{{5000.0f, 0.0f, 0.0f}});
    g.update(0.1);
    g.update(0.1);
    CHECK(count_owner(a, LogOp::SfxSteal, "EGL_Idle", 101) >= 1 && voice_gain(rig, a, i102) >= 0.0f);
    CHECK(voice_gain(rig, a, i101) < 0.0f);
    CHECK(a.sfx_live(idle3) && a.sfx_live(i101) && a.sfx_live(i102));
    CHECK(count_owner(a, LogOp::SfxStop, "", 101) == 0 && count_owner(a, LogOp::SfxStop, "", 102) == 0);
    // Distance attenuates: ship 100 (4900 away) is quieter than the ship at the listener.
    g.update(1.0);
    CHECK(voice_gain(rig, a, idle3) >= 0.0f && voice_gain(rig, a, idle3) < voice_gain(rig, a, i102));

    // Fog / hidden: the running loop is muted, not stopped; out of fog it is audible at once (no fade-in).
    const float g102 = voice_gain(rig, a, i102);
    const size_t before_fog = a.log().size();
    objs[102].audible = false;
    g.update(0.1);
    CHECK(a.sfx_live(i102) && voice_gain(rig, a, i102) == 0.0f);
    for (int i = 0; i < 20; ++i) g.update(0.1);
    objs[102].audible = true;
    g.update(0.0);
    CHECK(a.sfx_live(i102) && near(voice_gain(rig, a, i102), g102));
    for (size_t i = before_fog; i < a.log().size(); ++i) {
        const LogEntry& e = a.log()[i];
        CHECK(!(e.owner == 102 && (e.op == LogOp::SfxStop || e.op == LogOp::SfxStart)));
    }

    // Engines offline: both loops fade out; the next locomotor service brings idle back ("disabled engines are
    // heard as idle"). Engines online with idle already requested: nothing.
    g.engines_offline(102);
    st = g.engine_loop_state(102);
    CHECK(st && !st->idle && !st->moving && count_owner(a, LogOp::SfxStop, "EGL_Idle", 102, "fade") == 1);
    g.engine_service(102, false);
    CHECK(count_owner(a, LogOp::SfxStart, "EGL_Idle", 102) == 2 && g.engine_loop_state(102)->idle);
    g.engines_online(102);
    CHECK(count_owner(a, LogOp::SfxStart, "EGL_Idle", 102) == 2);

    // Suppression (+0x3B9): stops moving, idle and cinematic-focus loops with the given fade, clears the flags;
    // while set nothing starts; clearing it restarts nothing until a flag goes 0 -> 1.
    g.object_sfx(102, "EGL_Focus", objs[102].position);
    const size_t before_sup = a.log().size();
    g.suppress_engine_audio(102, 0.25f);
    st = g.engine_loop_state(102);
    CHECK(st && st->suppressed && !st->idle && !st->moving);
    CHECK(find_op(a, LogOp::SfxStop, "EGL_Focus", before_sup) >= 0 && find_op(a, LogOp::SfxStop, "EGL_Idle", before_sup) >= 0);
    g.engine_service(102, true);
    g.engine_mode_enter(102);
    CHECK(find_op(a, LogOp::SfxStart, "EGL_Move", before_sup) < 0 && find_op(a, LogOp::SfxStart, "EGL_Idle", before_sup) < 0);
    st = g.engine_loop_state(102);
    CHECK(st && !st->idle && !st->moving);
    g.clear_engine_audio_suppression(102);
    CHECK(find_op(a, LogOp::SfxStart, "EGL_Idle", before_sup) < 0);
    g.engine_service(102, false);
    CHECK(find_op(a, LogOp::SfxStart, "EGL_Idle", before_sup) >= 0);

    // Death: every sound of the object killed at once (fading ones too), no fade; the dead bit blocks restarts.
    g.object_moving(100, true);  // idle3 fading out, a moving loop fading in
    const uint32_t move3 = start_handle(a, "EGL_Move", 100);
    g.update(0.1);
    CHECK(a.sfx_live(idle3) && a.sfx_live(move3));
    const size_t before_death = a.log().size();
    CHECK(g.unit_death(100, "Death_Boom", objs[100].position) != 0);
    CHECK(!a.sfx_live(idle3) && !a.sfx_live(move3));
    for (size_t i = before_death; i < a.log().size(); ++i)
        if (a.log()[i].op == LogOp::SfxStop && a.log()[i].owner == 100) CHECK(a.log()[i].reason == "object_removed");
    CHECK(count_owner(a, LogOp::SfxStop, "EGL_Move", 100, "object_removed") == 1 &&
          count_owner(a, LogOp::SfxStop, "EGL_Idle", 100, "object_removed") == 1);
    st = g.engine_loop_state(100);
    CHECK(st && st->dead);
    const int starts100 = count_owner(a, LogOp::SfxStart, "", 100);
    g.engine_service(100, false);
    g.engine_mode_enter(100);
    g.engines_online(100);
    g.set_engine_moving_loop(100, true);
    g.object_moving(100, false);
    CHECK(count_owner(a, LogOp::SfxStart, "", 100) == starts100);

    // Removal / retreat-out (object left the battle): killed at once, no fade, object forgotten.
    g.object_moving(101, true);
    const uint32_t m101 = start_handle(a, "EGL_Move", 101);
    CHECK(m101 != 0 && a.sfx_live(i101));  // i101 fading out
    g.object_removed(101);
    CHECK(!a.sfx_live(i101) && !a.sfx_live(m101) && !g.engine_loop_state(101));
    CHECK(count_owner(a, LogOp::SfxStop, "EGL_Move", 101, "object_removed") == 1 &&
          count_owner(a, LogOp::SfxStop, "EGL_Move", 101, "fade") == 0);
    const int starts101 = count_owner(a, LogOp::SfxStart, "", 101);
    g.engine_mode_enter(101);
    CHECK(count_owner(a, LogOp::SfxStart, "", 101) == starts101);

    // No event: the requested-state flags still change.
    g.object_spawned(200, UnitSounds{}, {});
    st = g.engine_loop_state(200);
    CHECK(st && st->idle && !st->moving);
    g.engine_service(200, true);
    st = g.engine_loop_state(200);
    CHECK(st && !st->idle && st->moving && count_owner(a, LogOp::SfxStart, "", 200) == 0);

    // A start refused by a gate (Probability 0) keeps the flag set and is retried only on the next 0 -> 1.
    UnitSounds never;
    never.engine_idle_loop = "EGL_Never";
    g.object_spawned(300, never, {});
    CHECK(count_owner(a, LogOp::SfxReject, "EGL_Never", 300, "probability") == 1 && g.engine_loop_state(300)->idle);
    g.engine_mode_enter(300);
    g.engine_service(300, false);
    for (int i = 0; i < 5; ++i) g.update(0.1);
    CHECK(count_owner(a, LogOp::SfxReject, "EGL_Never", 300) == 1);
    g.engine_service(300, true);
    g.engine_service(300, false);
    CHECK(count_owner(a, LogOp::SfxReject, "EGL_Never", 300) == 2);

    // Unknown objects are ignored.
    g.engine_service(999, true);
    CHECK(!g.engine_loop_state(999) && count_owner(a, LogOp::SfxStart, "", 999) == 0);

    // leave_mode (stop all) forgets the objects.
    g.leave_mode();
    CHECK(!g.engine_loop_state(102) && a.live_instances() == 0);
    std::printf("engine loops: %zu log entries\n", a.log().size());
}

#ifdef ALAMO_AUDIO_HAVE_LUA
void test_lua_game() {
    Rig rig;
    AudioSystem& a = *rig.a;
    GameAudio g(a, rig.data, rig.rng);
    g.set_local_faction("Rebel");
    TacticalSetup land;
    land.environment = "Temperate";
    g.enter_tactical(land);
    using lua::PresentationArg;
    using lua::PresentationEvent;
    using lua::PresentationOp;
    auto boolean = [](bool v) {
        PresentationArg x;
        x.kind = PresentationArg::Kind::Bool;
        x.b = v;
        return x;
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
    add(PresentationOp::WeatherAudioPause, {boolean(true)});
    add(PresentationOp::SfxAllowEnemySightedVo, {mgr, boolean(false)});
    add(PresentationOp::MasterVolumeRestore, {});
    add(PresentationOp::WeatherAudioPause, {});
    a.set_bus_level(AudioBus::Master, 0.2f);
    uint32_t next = 0;
    LuaAudioResult r = apply_presentation_audio(g, ev, &next);
    CHECK(r.applied == 3 && r.unhandled == 0 && r.bad_args == 1 && next == 4);
    CHECK(g.ambience().paused() && g.enemy_spotted("Darth_Vader") == 0 && near(a.bus_level(AudioBus::Master), 0.8f));
    // The AudioSystem overload has no GameAudio: Weather_Audio_Pause x2 and Allow_Enemy_Sighted_VO unhandled.
    uint32_t n2 = 0;
    r = apply_presentation_audio(a, ev, &n2);
    CHECK(r.unhandled == 3 && r.applied == 1);
}
#endif

// Same inputs -> same log; only the free RNG is drawn.
std::string scripted(uint32_t seed, GameRandom& gr) {
    MemoryProvider tree = make_tree();
    AudioDatabase db;
    db.load(tree);
    GameAudioData data;
    data.load(tree);
    NullBackend nb{test_duration};
    gr.seed_free(seed);
    AudioSystem a(db, nb, gr.free());
    GameAudio g(a, data, gr.free());
    g.set_local_faction("Rebel");
    g.enter_galactic();
    TacticalSetup land;
    land.environment = "Temperate";
    UnitSounds us;
    us.response[0] = "Unit_Select";
    us.engine_idle_loop = "Engine_Idle";
    us.engine_moving_loop = "Engine_Move";
    us.ambient_moving = "Chatter_Moving";
    us.ambient_moving_min_seconds = 1.0f;
    us.ambient_moving_max_seconds = 4.0f;
    for (int f = 0; f < 1200; ++f) {
        if (f == 30) g.enter_tactical(land);
        if (f == 40) g.object_spawned(7, us, {1, 0, 1});
        if (f == 45) g.object_moving(7, true);
        if (f % 7 == 0) g.weapon_fire(7, "Gun_Fire", {static_cast<float>(f), 0, 0});
        if (f % 50 == 0) g.unit_response(7, us, UnitOrder::Select, {});
        if (f == 300) g.combat({true, true});
        if (f >= 400) g.set_weather("RAIN", static_cast<float>(f - 400) / 400.0f);
        g.sim_tick();
        g.update(1.0 / 30.0);
    }
    return a.log_text();
}

void test_determinism() {
    GameRandom g1(555, 0), g2(555, 0), g3(555, 0);
    const std::string l1 = scripted(9, g1), l2 = scripted(9, g2);
    CHECK(!l1.empty() && l1 == l2);
    CHECK(g1.synced().state() == 555 && g1.synced().draws() == 0);
    CHECK(scripted(10, g3) != l1);
    std::printf("game determinism: %zu log bytes, %llu free draws\n", l1.size(),
                static_cast<unsigned long long>(g1.free().draws()));
}

// ------------------------------------------------------------------ real data

using ::alamo::test::env_or_empty;

void test_real(const std::string& game, const std::string& root, bool is_eaw) {
    vfs::VfsConfig vc;
    vc.foc_root = root;
    if (is_eaw) vc.eaw_root = std::string();
    std::string err;
    auto v = vfs::Vfs::create(vc, &err);
    CHECK(v != nullptr);
    if (!v) return;
    AudioDatabase db;
    CHECK(db.load(*v));
    GameAudioData data;
    GameAudioLoadReport r;
    CHECK(data.load(*v, &r));
    std::string rep = "=== " + game + " (" + root + ")\n";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "weather loops %u, weather intermittents %u, ambient intermittents %u, ambient map sounds %u, "
                  "factions %u, diagnostics %zu\n",
                  r.weather_loops, r.weather_intermittents, r.ambient_intermittents, r.ambient_map_sounds, r.factions,
                  r.diagnostics.size());
    rep += buf;
    std::printf("real %s: %s", game.c_str(), buf);
    for (const std::string& d : r.diagnostics) rep += "  " + d + "\n";
    CHECK(r.weather_loops >= 15 && r.weather_intermittents >= 10 && r.factions >= 3);
    if (!is_eaw) CHECK(r.ambient_intermittents == 7);
    // STATION_HANGARS_SOUND §3.1: the vanilla engine loop presets (SFXEVENTSENGINES.XML).
    for (const char* preset : {"Preset_EGLC_Idle", "Preset_EGLC"}) {
        const uint32_t pi = db.find_sfx(preset);
        CHECK(pi != kNoEvent);
        if (pi == kNoEvent) continue;
        const SfxEvent& pe = db.sfx()[pi];
        CHECK(pe.is_3d && pe.max_instances == 2 && pe.play_count == -1 && pe.loop_fade_in_seconds == 0.5f &&
              pe.loop_fade_out_seconds == 1.0f);
        std::snprintf(buf, sizeof buf, "%s: max_instances %d, fade in %.2f s, out %.2f s, priority %d\n", preset,
                      pe.max_instances, static_cast<double>(pe.loop_fade_in_seconds),
                      static_cast<double>(pe.loop_fade_out_seconds), pe.priority);
        rep += buf;
    }

    // Every referenced SFX / music event resolves.
    uint32_t refs = 0, missing = 0;
    std::string missing_list;
    auto need_sfx = [&](const std::string& ev, const std::string& where) {
        if (ev.empty()) return;
        ++refs;
        if (db.find_sfx(ev) == kNoEvent) {
            ++missing;
            missing_list += "  sfx " + ev + " (" + where + ")\n";
        }
    };
    auto need_music = [&](const std::string& ev, const std::string& where) {
        if (ev.empty()) return;
        ++refs;
        if (db.find_music(ev) == kNoEvent) {
            ++missing;
            missing_list += "  music " + ev + " (" + where + ")\n";
        }
    };
    for (const auto& e : data.weather().loops) need_sfx(e.event, "Weather_SFXEvent_Loop " + e.weather);
    for (const auto& e : data.weather().intermittents) need_sfx(e.event, "Weather_SFXEvent_Intermittent " + e.weather);
    for (const auto& e : data.weather().ambient) need_sfx(e.event, "Ambient_SFXEvent_Intermittent " + e.environment);
    uint32_t bed_refs = 0, bed_missing = 0;
    for (const AmbientMapSound& a : data.ambient_map_sounds())
        for (const auto& e : a.entries) {
            ++bed_refs;
            bed_missing += db.find_sfx(e.event) == kNoEvent ? 1u : 0u;
        }
    const std::vector<std::string> envs = {"Temperate", "Arctic", "Desert", "Forest", "Swamp", "Volcanic", "Urban"};
    uint32_t faction_music = 0;
    for (const FactionAudio& f : data.factions()) {
        for (const auto& [tag, text] : f.fields) {
            const bool music = tag.find("Music") != std::string::npos;
            if (tag.find("List") != std::string::npos || tag.find("Vs_Faction") != std::string::npos ||
                tag.find("Spotted") != std::string::npos || tag.find("Weather_") != std::string::npos ||
                tag.find("Non_Hero") != std::string::npos || tag.find("Control") != std::string::npos ||
                tag.find("TSW") != std::string::npos || tag.find("Speech") != std::string::npos ||
                tag.find("Hardpoint") != std::string::npos) {
                // KEY, value rows: check the value.
                const auto fields = data::split_fields(text);
                if (fields.size() >= 2 && !fields[1].empty() && music) need_music(std::string(fields[1]), f.name + "." + tag);
                continue;
            }
            if (music) {
                need_music(text, f.name + "." + tag);
                ++faction_music;
            } else if (tag.find("SFX") != std::string::npos && text.find(',') == std::string::npos) {
                need_sfx(text, f.name + "." + tag);
            }
        }
    }
    uint32_t anim_sounds = 0, anim_missing = 0, anim_surfaces = 0;
    {
        // Walk every animation through crossed() over its whole range.
        std::set<std::string> seen_anim;
        std::vector<uint8_t> bytes;
        std::string text;
        if (v->read("Data\\XML\\AnimationSFXMaps.txt", bytes)) text.assign(bytes.begin(), bytes.end());
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const auto f = data::split_list(std::string_view(text).substr(pos, end - pos));
            pos = end + 1;
            if (f.size() < 2) continue;
            const std::string anim(f[1]);
            if (!seen_anim.insert(normalize_name(anim)).second) continue;
            for (const AnimSfxEntry* e : data.animation_sfx().crossed(anim, -1, 0x7FFFFFFF)) {
                if (e->kind == AnimSfxEntry::Kind::Surface) {
                    ++anim_surfaces;
                    continue;
                }
                ++anim_sounds;
                if (db.find_sfx(e->name) == kNoEvent) {
                    ++anim_missing;
                    missing_list += "  anim sfx " + e->name + " (" + anim + ")\n";
                }
            }
        }
    }
    CHECK(r.anim_sfx_entries > 500 && anim_sounds + anim_surfaces == r.anim_sfx_entries && anim_missing * 20 < anim_sounds);
    std::snprintf(buf, sizeof buf,
                  "references: %u (missing %u), faction music tags %u, map beds %u (missing %u), animation rows %u "
                  "(SOUND %u, missing events %u; SURFACE %u)\n",
                  refs, missing, faction_music, bed_refs, bed_missing, r.anim_sfx_entries, anim_sounds, anim_missing,
                  anim_surfaces);
    rep += buf;
    rep += missing_list;
    std::printf("real %s: %s", game.c_str(), buf);
    CHECK(refs > 100 && missing * 20 < refs);

    // Mode music for every faction that has a galactic music event, every land environment and space.
    GameSampleConfig sc;
    sc.sfx_megs = GameSampleSource::default_sfx_megs(*v, "English");
    GameSampleSource src(*v, sc);
    NullBackend nb([&](const SampleRef& ref) -> std::optional<double> {
        std::vector<uint8_t> bytes;
        if (!src.read(ref, bytes)) return std::nullopt;
        auto info = probe_audio(bytes);
        if (!info) return std::nullopt;
        return info->seconds();
    });
    SimRng rng{2026};
    AudioSystem a(db, nb, rng);
    GameAudio g(a, data, rng);
    uint32_t factions_checked = 0, mode_music_ok = 0, mode_music_total = 0;
    for (const FactionAudio& f : data.factions()) {
        if (f.get("Strategic_Map_Music_Event").empty()) continue;
        ++factions_checked;
        g.set_local_faction(f.name);
        g.enter_galactic();
        ++mode_music_total;
        mode_music_ok += a.current_music().empty() ? 0u : 1u;
        rep += f.name + ": galactic " + a.current_music();
        for (const std::string& e : envs) {
            TacticalSetup s;
            s.environment = e;
            g.enter_tactical(s);
            ++mode_music_total;
            mode_music_ok += a.current_music().empty() ? 0u : 1u;
            rep += ", " + e + " " + a.current_music();
            g.combat({true, false});
            ++mode_music_total;
            mode_music_ok += a.music_mode() == music_mode::kTacticalBattle && !a.current_music().empty() ? 1u : 0u;
            rep += "/" + a.current_music();
            g.leave_mode();
        }
        TacticalSetup sp;
        sp.land = false;
        g.enter_tactical(sp);
        ++mode_music_total;
        mode_music_ok += a.current_music().empty() ? 0u : 1u;
        rep += ", Space " + a.current_music() + "\n";
        g.leave_mode();
    }
    std::snprintf(buf, sizeof buf, "mode music: %u factions, %u/%u selections resolved\n", factions_checked, mode_music_ok,
                  mode_music_total);
    rep += buf;
    std::printf("real %s: %s", game.c_str(), buf);
    CHECK(factions_checked >= (is_eaw ? 2u : 3u) && mode_music_ok == mode_music_total);

    // A real weather session: heavy rain on a Temperate map for 60 s of wall time.
    a.clear_log();
    g.set_local_faction("Rebel");
    TacticalSetup temperate;
    temperate.environment = "Temperate";
    g.enter_tactical(temperate);
    CameraState cam;
    cam.look_at = {0.0f, 0.0f, 0.0f};
    cam.forward = {0.0f, 0.7f, -0.7f};
    cam.up = {0.0f, 0.0f, 1.0f};
    g.set_camera(cam);
    for (int f = 0; f < 1800; ++f) {
        g.set_weather(f < 900 ? "CLEAR" : "RAIN", f < 900 ? 0.0f : 1.0f);
        g.sim_tick();
        g.update(1.0 / 30.0);
    }
    const std::string loop = g.ambience().loop_event();
    int ambient_starts = 0, missing_samples = 0;
    for (const LogEntry& e : a.log()) {
        if (e.op == LogOp::SfxStart && e.event.find("Weather_Ambient") != std::string::npos) ++ambient_starts;
        if (e.op == LogOp::SfxVoice && e.reason == "sample_missing") ++missing_samples;
    }
    std::snprintf(buf, sizeof buf, "weather session: loop %s, %d ambient starts, %d missing samples, %zu log lines\n",
                  loop.c_str(), ambient_starts, missing_samples, a.log().size());
    rep += buf;
    std::printf("real %s: %s", game.c_str(), buf);
    if (!is_eaw) {
        CHECK(loop == "Weather_Ambient_Heavy_Rain_Loop" && a.sfx_live(g.ambience().loop_handle()));
        CHECK(ambient_starts >= 4 && missing_samples == 0);
        // The loop's shipped sample decodes.
        const SfxEvent& le = db.sfx()[db.find_sfx(loop)];
        std::vector<uint8_t> bytes;
        PcmBuffer pcm;
        CHECK(!le.samples.empty() && src.read(SampleRef{Bus::Sfx2D, le.samples[0]}, bytes) && decode_audio(bytes, pcm) &&
              pcm.frames() > 1000);
    }

    // Unit responses of every shipped unit type with a select line.
    data::GameDatabase gdb;
    gdb.load(*v);
    uint32_t types = 0, lines = 0, unknown = 0, started = 0;
    g.leave_mode();
    g.enter_galactic();  // no weather ambience drawing / refusing in the background
    a.clear_log();
    uint64_t id = 1000;
    for (const data::GameObjectRecord& o : gdb.game_objects()) {
        const UnitSounds us = unit_sounds_from_xml(o.element);
        if (us.response[0].empty()) continue;
        ++types;
        for (const std::string& s : us.response) {
            if (s.empty()) continue;
            ++lines;
            unknown += db.find_sfx(s) == kNoEvent ? 1u : 0u;
        }
        started += g.unit_response(++id, us, UnitOrder::Select, {}) != 0 ? 1u : 0u;
        g.update(0.0);  // the line's voice starts
        g.update(8.0);  // ... and ends, so overlap groups / Max_Instances do not refuse the next type
        g.update(0.0);
    }
    std::snprintf(buf, sizeof buf, "unit responses: %u types with SFXEvent_Select, %u lines (%u unknown events), %u selects started\n",
                  types, lines, unknown, started);
    rep += buf;
    std::printf("real %s: %s", game.c_str(), buf);
    std::map<std::string, uint32_t> reasons;
    for (const LogEntry& e : a.log())
        if (e.op == LogOp::SfxReject) {
            ++reasons[e.reason];
            if (e.reason != "probability") rep += "  refused " + e.event + " (" + e.reason + ")\n";
        }
    std::string rs = "  refused:";
    for (const auto& [why, n] : reasons) rs += " " + why + "=" + std::to_string(n);
    rep += rs + "\n";
    std::printf("real %s: %s\n", game.c_str(), rs.c_str());
    // Refusals come from the event data: Probability of chatter-style presets (Â§3.3), and the odd line still
    // playing when the next type shares it (FoC: Unit_Select_C3PO, Max_Instances 1).
    uint32_t data_refusals = 0;
    for (const auto& [why, n] : reasons) data_refusals += why == "probability" ? n : 0u;
    CHECK(types > 100 && unknown * 20 < lines && started + data_refusals <= types &&
          types - started - data_refusals <= types / 100 + 1);

#ifdef ALAMO_AUDIO_LOG_DIR
    std::ofstream out(std::string(ALAMO_AUDIO_LOG_DIR) + "/audio_game_" + game + "_report.txt", std::ios::binary);
    out << rep;
#endif
}

}  // namespace

int main() {
    test_data();
    test_ambience();
    test_conversation();
    test_music_modes();
    test_music_modes_gaps();
    test_hooks();
    test_engine_loops();
#ifdef ALAMO_AUDIO_HAVE_LUA
    test_lua_game();
#else
    std::printf("lua bridge: lua module not configured, skipped\n");
#endif
    test_determinism();
    const std::string foc = env_or_empty("ALAMO_FOC_DATA"), eaw = env_or_empty("ALAMO_EAW_DATA");
    if (foc.empty() && eaw.empty()) std::printf("real data: ALAMO_FOC_DATA / ALAMO_EAW_DATA not set, skipped\n");
    if (!foc.empty()) test_real("foc", foc, false);
    if (!eaw.empty()) test_real("eaw", eaw, true);
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_audio_game_tests: all passed\n");
    return 0;
}
