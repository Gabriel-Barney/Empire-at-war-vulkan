#include "game_movies.hpp"

#include <cstdio>

#ifdef ALAMO_GAME_HAVE_VIDEO
#include <cmath>
#include <cstdlib>
#include <vector>

#include "alamo/audio/audio_system.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/video/movie_db.hpp"
#include "alamo/video/video.hpp"
#else
namespace alamo::video {
class MoviePlayer {};  // complete stand-in for the owned pointer (no engine/video in this build)
}  // namespace alamo::video
#endif

namespace alamo::game {

MovieSlot::MovieSlot() = default;
MovieSlot::~MovieSlot() = default;  // owners call stop() first (it needs the audio host)

#ifdef ALAMO_GAME_HAVE_VIDEO

bool movies_available(std::string* err) { return video::ffmpeg_available(err); }

namespace {
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}
// "Event, frame, Event2, frame2" (the SFXEvent_Frame list shape [port: MusicEvent_Frame has no sample in the data]).
std::vector<std::pair<int64_t, std::string>> parse_frame_list(std::string_view text) {
    std::vector<std::string> tok;
    std::string cur;
    for (const char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) tok.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tok.push_back(std::move(cur));
    std::vector<std::pair<int64_t, std::string>> out;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) out.emplace_back(std::strtoll(tok[i + 1].c_str(), nullptr, 10), tok[i]);
    return out;
}
}  // namespace

bool MovieSlot::open(const GameData& data, GameAudioHost& audio, std::string_view name, bool loop, std::string* err) {
    stop(audio);
    if (!video::ffmpeg_available(err)) return false;
    audio::AudioSystem* sys = audio.system();
    // Movies.xml entry (by Name or Movie_File base name): its Movie_File is the path (e.g. Logo1 ->
    // Data\Art\Movies\Binked\LucasArtsLogo.bik), its Alpha decides the alpha plane and the draw (§7.3), Cannot_Skip the
    // skip keys. An entry with an empty Movie_File (FoC Logo3) has nothing to play. No entry: Binked\<name>.BIK, opaque
    // [port].
    video::MovieEntry entry;
    bool have_entry = false;
    bool credits_flag = false, halt_flag = false;
    std::vector<Cue> cues;
    {
        std::vector<uint8_t> xml;
        if (data.files->read("Data\\XML\\Movies.xml", xml)) {
            const data::XmlDocument doc = data::XmlDocument::parse(xml, "Movies.xml");
            const std::vector<video::MovieEntry> db = video::parse_movies(doc.root());
            if (const video::MovieEntry* m = video::find_movie(db, name)) {
                entry = *m;
                have_entry = true;
                // The music fields of the same record (the last <Movie> of that Name, like find_movie).
                for (data::XmlNode n : doc.root().children("Movie")) {
                    if (!ieq(n.attribute("Name"), entry.name)) continue;
                    credits_flag = data::parse_bool_or(n.child_text("Has_Credits"), false);
                    halt_flag = data::parse_bool_or(n.child_text("Halt_Game_Music"), false);
                    cues.clear();
                    for (auto& [frame, ev] : parse_frame_list(n.child_text("MusicEvent_Frame"))) cues.push_back({frame, std::move(ev)});
                }
            }
        }
    }
    if (have_entry && entry.file.empty()) {
        if (err) *err = "Movies.xml entry has no Movie_File";
        return false;
    }
    const std::string path = have_entry ? entry.file : "Data\\Art\\Movies\\Binked\\" + std::string(name) + ".BIK";
    video::OpenOptions oo = video::open_options_for(entry, video::PixelFormat::Rgba8, sys ? 0 : -1);
    auto dec = video::BinkDecoder::open(*data.files, path, oo, err);
    if (!dec) return false;
    const video::VideoInfo info = dec->info();
    const bool sound = sys && info.sample_rate > 0 && info.channels > 0;
    player_ = std::make_unique<video::MoviePlayer>(std::move(dec), sound ? video::MoviePlayer::Clock::Audio : video::MoviePlayer::Clock::Wall);
    if (sound) {  // the movie's PCM on the speech bus; the player is the master clock
        video::MoviePlayer* mp = player_.get();
        stream_ = sys->open_stream(static_cast<uint32_t>(info.sample_rate), static_cast<uint32_t>(info.channels), audio::AudioBus::Speech,
                                   [mp](float* o, size_t n) { return mp->read_audio(o, n); });
        if (stream_ == 0) {  // no stream support: wall clock, no sound
            oo.audio_track = -1;
            auto again = video::BinkDecoder::open(*data.files, path, oo, err);
            if (!again) {
                player_.reset();
                return false;
            }
            player_ = std::make_unique<video::MoviePlayer>(std::move(again), video::MoviePlayer::Clock::Wall);
        } else {
            player_->set_audio_latency(audio.output_latency_seconds());  // OI host: P3 video item 3
        }
    }
    player_->set_loop(loop);
    serial_ = 0;
    has_frame_ = false;
    alpha_ = entry.alpha;
    cannot_skip_ = entry.cannot_skip;
    has_credits_ = credits_flag;
    halt_music_ = halt_flag;
    cues_ = std::move(cues);
    next_cue_ = 0;
    fps_ = info.fps_num > 0 && info.fps_den > 0 ? static_cast<double>(info.fps_num) / static_cast<double>(info.fps_den) : 30.0;
    audio_ = &audio;
    // STRATEGIC_GAPS §7.4: the movie-music flag starts mode 14 music, Halt_Game_Music stops the game's music; both are
    // undone when the movie stops (GameAudioHost::movie_ended).
    if (has_credits_ || halt_music_) {
        audio.movie_started(has_credits_, halt_music_);
        music_started_ = true;
    }
    width_ = static_cast<uint32_t>(info.width > 0 ? info.width : 0);
    height_ = static_cast<uint32_t>(info.height > 0 ? info.height : 0);
    name_ = std::string(name);
    return true;
}

void MovieSlot::stop(GameAudioHost& audio) {
    if (stream_ && audio.system()) audio.system()->stop_stream(stream_);  // the mixer lets go of the player first
    stream_ = 0;
    player_.reset();
    if (music_started_) audio.movie_ended();
    music_started_ = false;
    audio_ = nullptr;
    cues_.clear();
    next_cue_ = 0;
    has_credits_ = halt_music_ = false;
    has_frame_ = false;
    cannot_skip_ = false;
    name_.clear();
}

bool MovieSlot::update(double dt, ui::HudRenderer& hud, uint16_t page) {
    if (!player_) return false;
    player_->update(dt);
    if (audio_ && !cues_.empty()) {  // music cues (mode 17) keyed to the movie's frame clock; a loop restarts the list
        const int64_t frame = static_cast<int64_t>(std::floor(player_->time_s() * fps_));
        if (next_cue_ > 0 && frame < cues_[next_cue_ - 1].frame) next_cue_ = 0;
        while (next_cue_ < cues_.size() && cues_[next_cue_].frame <= frame) audio_->movie_cue(cues_[next_cue_++].event);
    }
    const video::VideoFrame* vf = player_->current_frame();
    if (vf && player_->frame_serial() != serial_ && vf->plane_count > 0) {
        serial_ = player_->frame_serial();
        has_frame_ = hud.set_image_rgba(page, static_cast<uint32_t>(vf->width), static_cast<uint32_t>(vf->height), vf->data);
    }
    return !player_->finished();
}

bool MovieSlot::playing() const { return player_ && !player_->finished(); }

#else  // no engine/video: movies are skipped

bool movies_available(std::string* err) {
    if (err) *err = "built without engine/video";
    return false;
}
bool MovieSlot::open(const GameData&, GameAudioHost&, std::string_view, bool, std::string* err) {
    if (err) *err = "built without engine/video";
    return false;
}
void MovieSlot::stop(GameAudioHost&) {
    player_.reset();
    stream_ = 0;
    music_started_ = false;
    audio_ = nullptr;
    cues_.clear();
    next_cue_ = 0;
    fps_ = 30.0;
    serial_ = 0;
    has_frame_ = false;
    name_.clear();
}
bool MovieSlot::update(double, ui::HudRenderer&, uint16_t) { return false; }
bool MovieSlot::playing() const { return false; }

#endif

}  // namespace alamo::game
