// alamo_game_audio_host_tests: GameAudioHost's movie music (STRATEGIC_GAPS §7.3 / §7.4), data-free: a synthetic audio tree
// in memory and the null backend. The movie-music flag (Movies.xml Has_Credits) starts mode 14 music and the movie's end
// restores the music it replaced; Halt_Game_Music stops the music for the movie and brings the mode music back after it;
// a MusicEvent_Frame cue is mode 17. Movies without those flags never reach the host (MovieSlot only calls it for them).
// Without engine/audio the host is inactive: the test then only checks that every call is a harmless no-op.
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/game/game_audio_host.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_GAME_HAVE_AUDIO
#include "alamo/audio/audio_system.hpp"
#endif

using namespace alamo;
using namespace alamo::game;

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
  <SFXEvent Name="GUI_Click"><Is_2D>Yes</Is_2D><Is_3D>No</Is_3D><Is_GUI>Yes</Is_GUI><Samples>CLICK.WAV</Samples></SFXEvent>
</SFXEvents>
)";
const char* kMusic = R"(<?xml version="1.0"?>
<MusicEvents>
  <MusicEvent Name="Gal_Rebel"><Files>G1.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Game_Credits_Music_Event"><Files>C1.MP3, C2.MP3, C3.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Main_Menu_Music_Event"><Files>MENU.MP3</Files><Loop>Yes</Loop></MusicEvent>
  <MusicEvent Name="Movie_Cue"><Files>CUE.MP3</Files></MusicEvent>
</MusicEvents>
)";
const char* kFactions = R"(<?xml version="1.0" ?>
<Factions>
  <Faction Name="Rebel"><Strategic_Map_Music_Event> Gal_Rebel </Strategic_Map_Music_Event></Faction>
</Factions>
)";

[[maybe_unused]] std::unique_ptr<MemoryProvider> make_tree(bool credits_music) {
    auto p = std::make_unique<MemoryProvider>();
    p->add("Data\\XML\\SFXEventFiles.xml", "<SFXEvent_Files><File>SFXEventsTest.xml</File></SFXEvent_Files>");
    p->add("Data\\XML\\SFXEventsTest.xml", kSfx);
    std::string music = kMusic;
    if (!credits_music) {  // the event the credits movie asks for is missing from the database
        const size_t a = music.find("  <MusicEvent Name=\"Game_Credits_Music_Event\"");
        const size_t b = music.find('\n', a);
        music.erase(a, b - a + 1);
    }
    p->add("Data\\XML\\MusicEvents.xml", music);
    p->add("Data\\XML\\SpeechEvents.xml", "<SpeechEvents/>");
    p->add("Data\\XML\\Audio.xml", "<Audio/>");
    p->add("Data\\XML\\FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    p->add("Data\\XML\\Factions.xml", kFactions);
    return p;
}

#ifdef ALAMO_GAME_HAVE_AUDIO
std::string cur(GameAudioHost& h) { return h.system()->current_music(); }
uint8_t mode(GameAudioHost& h) { return h.system()->music_mode(); }

void test_movie_music() {
    GameData data;
    data.files = make_tree(true);
    GameAudioHost h;
    std::string err;
    CHECK(h.init(data, AudioBackendKind::Null, 7, &err));
    CHECK(h.active() && h.system());
    if (!h.active()) return;
    h.music(MusicScene::Galactic, "Rebel");
    CHECK(cur(h) == "Gal_Rebel" && mode(h) == audio::music_mode::kGalactic);

    // The movie-music flag (Has_Credits): mode 14, the credits event; the movie ending restores the galactic music.
    h.movie_started(true, false);
    CHECK(cur(h) == "Game_Credits_Music_Event" && mode(h) == audio::music_mode::kMovie && mode(h) == 14);
    CHECK(h.stats().movie_music == 1);
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel" && mode(h) == audio::music_mode::kGalactic);
    CHECK(h.stats().movie_restores == 1);
    h.movie_ended();  // nothing to undo: no change
    CHECK(cur(h) == "Gal_Rebel" && h.stats().movie_restores == 1);

    // Halt_Game_Music: silence while the movie plays, the mode music comes back with the movie's end.
    h.movie_started(false, true);
    CHECK(cur(h).empty() && h.stats().movie_halts == 1);
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel" && mode(h) == audio::music_mode::kGalactic);

    // Halt wins over the movie-music flag (a halted movie plays no music of its own).
    h.movie_started(true, true);
    CHECK(cur(h).empty() && h.stats().movie_music == 1);
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel");

    // A music cue (17) replaces the music; the movie's end restores it.
    h.movie_cue("Movie_Cue");
    CHECK(cur(h) == "Movie_Cue" && mode(h) == audio::music_mode::kMovieCue && mode(h) == 17);
    CHECK(h.stats().movie_cues == 1);
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel" && mode(h) == audio::music_mode::kGalactic);
    h.movie_cue("");  // empty names and unknown events change nothing
    h.movie_cue("No_Such_Event");
    CHECK(cur(h) == "Gal_Rebel");
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel");

    // The movie's music already gave way to something else (the new mode's music): ending the movie leaves it alone.
    h.movie_started(true, false);
    CHECK(mode(h) == audio::music_mode::kMovie);
    h.music(MusicScene::MainMenu, "Rebel");  // a mode change replaces the movie music
    h.movie_ended();
    CHECK(mode(h) != audio::music_mode::kMovie);
}

void test_missing_credits_event() {
    GameData data;
    data.files = make_tree(false);
    GameAudioHost h;
    CHECK(h.init(data, AudioBackendKind::Null, 7));
    if (!h.active()) return;
    h.music(MusicScene::Galactic, "Rebel");
    h.movie_started(true, false);  // no such event: the music stays, nothing to restore
    CHECK(cur(h) == "Gal_Rebel" && h.stats().movie_music == 0);
    h.movie_ended();
    CHECK(cur(h) == "Gal_Rebel" && h.stats().movie_restores == 0);
}
#endif

void test_inactive() {
    GameData data;
    GameAudioHost h;
    CHECK(h.init(data, AudioBackendKind::Off, 1));
    CHECK(!h.active());
    h.movie_started(true, true);
    h.movie_cue("Movie_Cue");
    h.movie_ended();
    CHECK(h.stats().movie_music == 0 && h.stats().movie_cues == 0 && h.stats().movie_halts == 0);
}

// ccpgu t01 (SKIRMISH_SETUP 4.2 announcer): the countdown lines at 60 / 30 / 15 / 5 s (x 30 frames), by the leader's relation
// to the local player; none for a Neutral observer or while not armed. Both builds (the decision is presentation logic only).
void test_control_point_lines() {
    using H = GameAudioHost;
    const std::string_view owned30 = "SFXEvent_HUD_Reinforcement_Point_Owned_30_Seconds";
    // Arming at 900 (the shipped 30 s), seen as 900 or, when the first countdown step fell in the same tick, 899.
    CHECK(H::control_point_line(-1, 900, 1, 1, false) == owned30);
    CHECK(H::control_point_line(-1, 899, 1, 1, false) == owned30);
    CHECK(H::control_point_line(-1, 899, 2, 1, false) == "SFXEvent_HUD_Reinforcement_Point_Ally_Owned_30_Seconds");
    CHECK(H::control_point_line(-1, 899, 2, 1, true) == "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_30_Seconds");
    CHECK(H::control_point_line(-1, 899, 2, -1, true).empty());  // no local player
    CHECK(H::control_point_line(5, -1, -1, 1, false).empty());   // disarmed
    // Counting down: each mark once, on the tick that reaches it.
    CHECK(H::control_point_line(899, 898, 1, 1, false).empty());
    CHECK(H::control_point_line(451, 450, 1, 1, false) == "SFXEvent_HUD_Reinforcement_Point_Owned_15_Seconds");
    CHECK(H::control_point_line(450, 449, 1, 1, false).empty());
    CHECK(H::control_point_line(151, 150, 2, 1, true) == "SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_05_Seconds");
    CHECK(H::control_point_line(1801, 1800, 1, 1, false) == "SFXEvent_HUD_Reinforcement_Point_Owned_60_Seconds");
    // A re-arm (900 again after a capture) plays the 30 s line again.
    CHECK(H::control_point_line(300, 900, 1, 1, false) == owned30);
    // Over the shipped 30 s countdown exactly three lines: 30 s at arming, 15 s, 5 s.
    int lines = 0, prev = -1;
    for (int t = 900; t >= 0; --t) {
        lines += H::control_point_line(prev, t, 1, 1, false).empty() ? 0 : 1;
        prev = t;
    }
    CHECK(lines == 3);
}

}  // namespace

int main() {
    test_inactive();
    test_control_point_lines();
#ifdef ALAMO_GAME_HAVE_AUDIO
    test_movie_music();
    test_missing_credits_event();
#endif
    std::printf(g_fail == 0 ? "game audio host tests passed\n" : "%d game audio host check(s) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
