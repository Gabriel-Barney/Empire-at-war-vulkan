// alamo_game Bink movies (engine/video): one MovieSlot per screen use - the full-screen slot (intro / logo movies at
// startup, Lua Play_Bink_Movie) and the HUD advisor slot (200 x 200 talking-head loops with alpha). Presentation only:
// Lua movie blocks finish on sim frames (lua::PresentationConfig::movie_frames), never on playback, so a movie never
// changes what the sim computes. Sound: an AudioSystem stream on the speech bus drives Clock::Audio; without stream
// support (or without audio) the player runs on Clock::Wall and the movie is silent.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/game/game_audio_host.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/ui/hud_renderer.hpp"

namespace alamo::video {
class MoviePlayer;
}

namespace alamo::game {

// FFmpeg's Bink decoder present (engine/video loads the DLLs at run time). False + err when movies are skipped.
bool movies_available(std::string* err = nullptr);

class MovieSlot {
public:
    MovieSlot();
    ~MovieSlot();
    MovieSlot(const MovieSlot&) = delete;
    MovieSlot& operator=(const MovieSlot&) = delete;

    // Opens Data\Art\Movies\Binked\<name>.BIK (any case). `loop` restarts it at the end. False + err.
    bool open(const GameData& data, GameAudioHost& audio, std::string_view name, bool loop, std::string* err = nullptr);
    void stop(GameAudioHost& audio);  // the audio stream is stopped before the player goes away
    // Advances by dt and uploads a new frame to `page` (HudRenderer::set_image_rgba). False once finished / stopped.
    bool update(double dt, ui::HudRenderer& hud, uint16_t page);
    bool playing() const;
    bool has_frame() const { return has_frame_; }
    const std::string& name() const { return name_; }
    bool audio_clock() const { return stream_ != 0; }
    // Movies.xml Alpha of the open movie (STRATEGIC_GAPS §7.3): decoded with the alpha plane, drawn on PrimAlpha
    // (ui::add_movie_quad alpha = true); false = opaque (no entry, or Alpha No).
    bool alpha() const { return alpha_; }
    // Movies.xml Cannot_Skip of the open movie.
    bool cannot_skip() const { return cannot_skip_; }
    // Movies.xml music flags of the open movie (STRATEGIC_GAPS §7.3 / §7.4): Has_Credits (+0xD0, the movie-music flag ->
    // music mode 14), Halt_Game_Music (+0xD4) and the MusicEvent_Frame cues (mode 17, fired by update()).
    bool has_credits() const { return has_credits_; }
    bool halt_music() const { return halt_music_; }
    // Frame size of the open movie (0 when none).
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    std::unique_ptr<video::MoviePlayer> player_;
    uint32_t stream_ = 0;
    uint64_t serial_ = 0;
    bool has_frame_ = false;
    bool alpha_ = false;
    bool cannot_skip_ = false;
    bool has_credits_ = false, halt_music_ = false;
    bool music_started_ = false;                    // this slot told the audio host a movie began (stop() ends it)
    GameAudioHost* audio_ = nullptr;                // valid between open() and stop()
    struct Cue {
        int64_t frame = 0;
        std::string event;
    };
    std::vector<Cue> cues_;                         // MusicEvent_Frame, in file order
    size_t next_cue_ = 0;
    double fps_ = 30.0;
    uint32_t width_ = 0, height_ = 0;
    std::string name_;
};

}  // namespace alamo::game
