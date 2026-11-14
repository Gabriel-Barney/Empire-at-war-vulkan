// Audio event player: everything the original builds above Miles (docs/re/AUDIO.md §0): SFX event
// gates and instance state machine, 2D/3D voice pools with priority stealing, 3D distance model,
// listener, four buses with speech ducking, the music playlist/crossfade player with its mode
// switches, and the speech FIFO. Backend-agnostic: the miniaudio backend plays sound, the null backend
// simulates voice lengths so tests get a deterministic event log.
//
// Section references (§) are to docs/re/AUDIO.md. Tags: [port] = a choice of this port where the
// original differs or is silent; [M]/[L] = the spec's own confidence for the rule implemented.
//
// Threading: one owner thread (the main loop's audio step, MAIN_LOOP.md step 14: wall time, only while
// the application is active). Not thread-safe. Sim code may queue requests but never reads audio back
// (§6.1); parallel sim jobs must submit their requests in index order.
//
// Randomness: only the FREE (unsynchronized) RNG instance (alamo::GameRandom::free(), MAIN_LOOP §4.1).
//
// Frame-rate dependence (§7) [port]: the original steps the SFX stop fade by (1/30)/fade_seconds per
// audio update; here it is stepped by dt/fade_seconds of wall time (same duration at 30 updates/s).
// Delays, music/speech ramps and bus ramps are wall time as in the original; the music peace timer is
// counted in sim ticks (music_sim_tick()).
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/audio/audio_db.hpp"
#include "alamo/audio/pcm_stream.hpp"
#include "alamo/audio/sample_source.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::audio {

struct VoiceParams {
    float gain = 1.0f;   // linear, all factors applied
    float pan = 0.0f;    // -1 left .. +1 right
    float pitch = 1.0f;  // playback-rate factor
    bool loop = false;
};

class VoiceBackend {
public:
    virtual ~VoiceBackend() = default;
    // Starts (or restarts) `voice` with `sample`. false = sample missing / undecodable.
    virtual bool start(uint32_t voice, const SampleRef& sample, const VoiceParams& p) = 0;
    // Loads (decodes / caches) `sample` without taking a voice. false = missing / undecodable: the SFX stage is skipped
    // before a voice is taken or stolen (AUDIO.md §1.7, §5; b0282). Default: assume present (start() decides).
    virtual bool load(const SampleRef& sample) {
        (void)sample;
        return true;
    }
    virtual void set(uint32_t voice, const VoiceParams& p) = 0;
    virtual void stop(uint32_t voice) = 0;
    // false once a non-looping voice reached its end (or was stopped / never started).
    virtual bool playing(uint32_t voice) const = 0;
    // Called once per AudioSystem::update before voices are polled.
    virtual void advance(double seconds) = 0;
    // Streaming PCM voice (pcm_stream.hpp): the backend resamples `format` to its device format and calls
    // `pull` for what the device consumes. set/stop/playing work on the voice id as for samples; a stream
    // plays (silence on underrun) until stopped. Default: unsupported (false).
    virtual bool start_stream(uint32_t voice, const PcmStreamFormat& format, PcmPullFn pull, const VoiceParams& p) {
        (void)voice;
        (void)format;
        (void)pull;
        (void)p;
        return false;
    }
    // OI host (P3 video item 3): seconds between a pull and its sound leaving the device (the output buffer: period size
    // x periods / rate). A movie's audio clock subtracts it. Default 0 (headless / no device).
    virtual double output_latency_seconds() const { return 0.0; }
};

// Headless device for streams: advance(s) consumes round(s x sample_rate) device frames (fraction carried).
struct NullDevice {
    uint32_t sample_rate = 44100;
    uint32_t channels = 2;
    size_t capture_limit = size_t{1} << 22;  // floats kept per stream (post-gain device output)
};

// Headless backend: no device. A voice lasts duration(sample) / pitch seconds of advance() time; a stream
// voice is pulled through StreamResampler by advance() as a device would.
class NullBackend final : public VoiceBackend {
public:
    using DurationFn = std::function<std::optional<double>(const SampleRef&)>;  // nullopt = missing
    explicit NullBackend(DurationFn duration, NullDevice device = {})
        : duration_(std::move(duration)), device_(device) {}

    bool start(uint32_t voice, const SampleRef& sample, const VoiceParams& p) override;
    bool load(const SampleRef& sample) override { return duration_ && duration_(sample).has_value(); }
    void set(uint32_t voice, const VoiceParams& p) override;
    void stop(uint32_t voice) override;
    bool playing(uint32_t voice) const override;
    void advance(double seconds) override;
    bool start_stream(uint32_t voice, const PcmStreamFormat& format, PcmPullFn pull, const VoiceParams& p) override;

    struct StreamVoice {
        std::unique_ptr<StreamResampler> resampler;
        VoiceParams params;
        std::vector<float> captured;  // device-format output x gain (up to NullDevice::capture_limit floats)
        uint64_t device_frames = 0;   // device frames consumed
    };
    const StreamVoice* stream(uint32_t id) const;
    const NullDevice& device() const { return device_; }

    struct Voice {
        SampleRef sample;
        VoiceParams params;
        double length = 0.0;    // seconds at pitch 1
        double position = 0.0;  // seconds of sample consumed
        bool active = false;
    };
    const Voice* voice(uint32_t id) const;
    uint32_t active_count() const;

private:
    DurationFn duration_;
    NullDevice device_;
    std::map<uint32_t, Voice> voices_;  // id-ordered: deterministic
    std::map<uint32_t, StreamVoice> streams_;
    double frame_carry_ = 0.0;
};

// Buses (§4): 0 master multiplies 1..3.
enum class AudioBus : uint8_t { Master = 0, Sfx = 1, Speech = 2, Music = 3 };

struct AudioConfig {
    uint32_t max_voices_2d = 16;  // §1.7
    uint32_t max_voices_3d = 32;
    float max_distance = 15000.0f;  // §1.7: constant max distance per 3D voice
    float speech_duck_cap = 0.3f;   // §1.6 / §3.2
    float default_bus_level = 0.8f; // §4
    std::string language = "English";  // §1.3 localization (captured per instance)
    // [port] Most entries the event log (log()) keeps; reaching it drops the oldest half, 0 keeps none. Unlimited by
    // default (tests read the whole log). The live game bounds it (kGameAudioLogLimit): every SFX start / reject / steal
    // is logged, about 60,000 entries per second in a space battle, and an unbounded log grew by 1.5x on the frame thread
    // (copies of 100+ ms) and leaked about 540 MB per minute (docs/PERFORMANCE.md "Audio event log hitches").
    size_t log_limit = static_cast<size_t>(-1);
};

struct Listener {
    Vec3 position{};
    Vec3 forward{0.0f, 0.0f, 1.0f};  // §1.8 defaults with no mode
    Vec3 up{0.0f, 1.0f, 0.0f};
};
// §1.8: tactical/galactic listener = look-at - forward x pullback; during a cinematic camera the
// listener is the camera position (pullback 0).
Listener listener_from_camera(const Vec3& camera_position, const Vec3& look_at, const Vec3& forward,
                              const Vec3& up, float pullback, bool cinematic);

// Language suffix for localized file names (§1.3); "" for English / unknown.
std::string_view language_suffix(std::string_view language);
// Replaces a trailing _ENG.WAV / _ENG.MP3 (case-insensitive) with the language's suffix.
std::string localize_file(std::string_view file, std::string_view language);

// How an SFX start was requested (§1.2 entry points -> instance types 0..3).
struct SfxPlay {
    enum class Kind : uint8_t {
        Ui,        // by name/event, 2D, no object (type 0)
        Object,    // attached to an object: 3D-attached if Is_3D (type 3) else 2D-attached (type 1)
        Position,  // 3D at a fixed world position (type 2)
    };
    Kind kind = Kind::Ui;
    uint64_t owner = 0;       // Object: game object id
    Vec3 position{};          // Position: the point; Object: initial position (updated via the object query)
    bool force = false;       // skips gates 4..8
    uint32_t pump = 0;        // GUI entry points: run this many SFX updates now (same-frame clicks)
    float fade_in_seconds = 0.0f;  // callers starting loops pass Loop_Fade_In_Seconds (§1.6)
};

// Music modes (MusicEventManager +0xc, AUDIO.md §2.3, STRATEGIC_GAPS §7.4).
//   0 debug event list, 1 galactic, 4 production complete (Build_Music_Completed, resume previous), 5 battle
//   pending, 6 tactical ambient (also Hutt Palace / Cantina selection, forced), 7 tactical battle, 8 tactical super
//   weapon activation, 9 / 10 own / enemy object type music, 11 splash screen, 12 Lua Play_Music, 13 front-end
//   menus (refused while the attract lock is set, unless forced), 14 movie music (a looping playlist stops after one
//   pass and resumes the previous event), 15 battle load screen, 16 battle-end dialog (win / lose), 17 movie cues.
//   Unforced Resume_Mode_Based_Music does nothing in 8, 11 and 12.
namespace music_mode {
inline constexpr uint8_t kNone = 0, kDebugList = 0, kGalactic = 1, kBuildComplete = 4, kBattlePending = 5,
                         kTacticalAmbient = 6, kTacticalBattle = 7, kSuperWeapon = 8, kOwnObject = 9, kEnemyObject = 10,
                         kSplash = 11, kLua = 12, kFrontEnd = 13, kMovie = 14, kLoadScreen = 15, kBattleEnd = 16,
                         kMovieCue = 17;
inline constexpr uint8_t kLocked = kFrontEnd, kOnePass = kMovie;  // older names
}

enum class LogOp : uint8_t {
    SfxStart,    // accepted (instance created)
    SfxReject,   // refused at a gate; reason
    SfxCycle,    // state 1: volume/pan/pitch/delays drawn
    SfxVoice,    // a stage started on a voice (sample, gain)
    SfxNoVoice,  // a stage found no voice (rest of the cycle skipped)
    SfxSteal,    // a voice was taken from this instance
    SfxStop,     // stopped by the game (reason)
    SfxEnd,      // removed after its last cycle
    SfxChain,    // Chained_SFXEvent started
    MusicPlay,   // event accepted by Play()
    MusicRefuse,
    MusicTrack,  // a file started
    MusicStop,
    SpeechQueue,
    SpeechStart,
    SpeechEnd,
    SpeechKill,
    StreamOpen,  // stream voice opened (gain); reason "unsupported" when the backend refused
    StreamStop,
};
const char* log_op_name(LogOp op);

struct LogEntry {
    int64_t time_us = 0;
    LogOp op = LogOp::SfxStart;
    std::string event;
    std::string sample;
    std::string reason;
    uint32_t handle = 0;
    uint32_t voice = 0;
    uint64_t owner = 0;
    int32_t volume = 0, pan = 0, pitch = 0, predelay_ms = 0, postdelay_ms = 0;
    float gain = 0.0f;
    std::string to_string() const;
};

class AudioSystem {
public:
    // db, backend and free_rng must outlive the system. free_rng = GameRandom::free().
    AudioSystem(const AudioDatabase& db, VoiceBackend& backend, SimRng& free_rng, AudioConfig cfg = {});
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // ---- SFX (§1). Returns an instance id, 0 if a gate refused it (see the log).
    uint32_t play_sfx(std::string_view event, const SfxPlay& p = {});
    uint32_t play_sfx(uint32_t event_index, const SfxPlay& p);
    void stop_sfx(uint32_t handle, float fade_seconds = 0.0f);
    void stop_object_sfx(uint64_t owner, std::string_view event, float fade_seconds = 0.0f);  // Lua Stop_SFX_Event
    void object_removed(uint64_t owner);  // death/deletion kills every attached instance (§1.5)
    void stop_all_sfx();                  // mode push/pop, load, quit
    bool sfx_live(uint32_t handle) const;

    // Object position/visibility for attached instances, queried every update (§1.4, §1.6).
    struct ObjectAudio {
        Vec3 position{};
        bool audible = true;  // false: hidden / fogged -> visibility factor 0 (keeps running silently)
        // Hidden / stealth / garrison (§1.6): muted even during a cinematic, which skips only the fog test. A query
        // that leaves it false and reports audible = false means "fogged" (unmuted by a cinematic).
        bool hidden = false;
    };
    void set_object_query(std::function<std::optional<ObjectAudio>(uint64_t owner)> q) { object_query_ = std::move(q); }
    // Gate 9: 3D-positional starts fail when the point is under the local player's fog (not in cinematics).
    void set_position_fogged(std::function<bool(const Vec3&)> f) { position_fogged_ = std::move(f); }

    // ---- Permission flags (Lua SFXManager.Allow_*, default on; gate 3).
    void allow_localized_sfx(bool on) { allow_localized_ = on; }
    void allow_unit_response_vo(bool on) { allow_unit_response_vo_ = on; }
    void allow_hud_vo(bool on) { allow_hud_vo_ = on; }
    void allow_ambient_vo(bool on) { allow_ambient_vo_ = on; }
    // ---- Game state the gates and gain read.
    void set_cinematic(bool on) { cinematic_ = on; }              // gate 2, gate 9, visibility
    void set_gui_only(bool on) { gui_only_ = on; }                // gate 4 (flag +0x94 AND modal dialog open)
    void set_modal_dialog(bool on) { modal_dialog_ = on; }        // disables speech ducking
    void set_hud_vo_blocked(bool on) { hud_vo_blocked_ = on; }    // gate 5 (tutorial flag in a tactical mode)
    void set_3d_scale(float s) { scale_3d_ = s; }                 // 0 behind the battle-load screen
    void set_attract_mode(bool on) { attract_ = on; }             // 3D voices silent, no mode music

    // ---- Listener + mode distance factors (§1.7, §1.8).
    void set_mode(AudioMode m) { mode_ = m; }
    AudioMode mode() const { return mode_; }
    void set_listener(const Listener& l) { listener_ = l; }
    const Listener& listener() const { return listener_; }

    // ---- Buses (§4).
    void set_bus_level(AudioBus bus, float level, float ramp_seconds = 0.0f);
    float bus_level(AudioBus bus) const { return buses_[static_cast<int>(bus)].level; }
    float bus_gain(AudioBus bus) const;  // level x master (master itself: level)
    // §4 saved master level for Lua Master_Volume_Restore. save_master_volume() records the current
    // master target (the options code calls it after applying the profile's VolumeMaster); restore ramps
    // the master bus back to it. [M] who saves in the original is not documented; the saved level
    // starts at AudioConfig::default_bus_level.
    void save_master_volume() { saved_master_ = buses_[0].target; }
    void master_volume_restore(float ramp_seconds = 0.0f) { set_bus_level(AudioBus::Master, saved_master_, ramp_seconds); }
    float saved_master_volume() const { return saved_master_; }

    // ---- Music (§2).
    using MusicResolver = std::function<std::string(uint8_t mode, SimRng& free_rng)>;
    // The game's mode music table (faction events, map environment lists, super-weapon variants).
    void set_music_resolver(MusicResolver r) { music_resolver_ = std::move(r); }
    bool play_music(std::string_view event, uint8_t mode = music_mode::kLua, bool resume_previous = false,
                    bool force = false);
    void stop_all_music();
    // [port] The playing music entry was started in `mode` and its owner (a movie, modes 14 / 17) ends: it stops now and
    // the event it replaced restarts in its own mode (forced). False when the current entry is in another mode.
    bool end_music_mode(uint8_t mode);
    void set_music_enabled(bool on) { music_enabled_ = on; }
    void set_music_lock(bool on) { music_lock_ = on; }
    void set_base_music_mode(uint8_t mode) { base_music_mode_ = mode; }  // galactic 1 / tactical 6
    void resume_mode_based_music(bool force);
    void notify_combat(bool involves_local_player);  // Land/Space slot-76 notification (§2.3)
    void music_sim_tick();                           // peace counter, once per sim tick
    uint8_t music_mode() const { return music_mode_; }
    std::string current_music() const;

    // ---- Speech (§3).
    bool play_speech(std::string_view event) { return queue_speech(event) != 0; }
    // Queues a speech event (FIFO) and returns a ticket (non-zero; 0 = unknown event) that identifies
    // this queue entry for speech_pending / cancel_speech (conversations, holograms).
    uint32_t queue_speech(std::string_view event);
    bool speech_pending(uint32_t ticket) const;  // queued or playing
    // Removes one entry: a waiting one silently leaves the queue, the playing one is stopped. Both run
    // the done callbacks with killed = true. [port] the original only has Stop_All_Speech.
    void cancel_speech(uint32_t ticket);
    void stop_all_speech();
    bool speech_playing() const { return speech_voice_ != 0; }
    std::string current_speech() const;
    size_t speech_queue_size() const { return speech_queue_.size(); }
    // Called when a speech ends (killed = Stop_All_Speech / mode change).
    void on_speech_done(std::function<void(const std::string& event, bool killed)> cb) {
        speech_done_.push_back(std::move(cb));
    }
    // Called when a speech stream starts (§3.4 subtitles: the hologram / story paths show the event's
    // Text_ID while it plays). `file` is the localized file actually streamed.
    void on_speech_start(std::function<void(const SpeechEvent& event, const std::string& file)> cb) {
        speech_start_.push_back(std::move(cb));
    }

    // ---- Streaming PCM voices (movie sound; pcm_stream.hpp). [port] the original plays Bink audio through
    // Miles; its bus and ducking are not in AUDIO.md. Defaults: speech/dialog bus, and while the stream is
    // open SFX voices are capped exactly as during speech (§1.6 / §3.2, not with a modal dialog open).
    struct StreamParams {
        PcmStreamFormat format;           // source rate / channels (the backend resamples to the device)
        AudioBus bus = AudioBus::Speech;  // gain = bus gain (x master) x volume; Sfx-bus streams are capped by speech
        float volume = 1.0f;              // 0..1
        bool ducks_sfx = true;
    };
    // Opens a stream voice; the backend calls `pull` (on its mixing thread) for what the device consumes,
    // until stop_stream. Returns a handle, 0 if the backend has no stream support or the format is empty.
    uint32_t open_stream(const StreamParams& params, PcmPullFn pull);
    uint32_t open_stream(uint32_t sample_rate, uint32_t channels, AudioBus bus, PcmPullFn pull) {
        StreamParams p;
        p.format = PcmStreamFormat{sample_rate, channels};
        p.bus = bus;
        return open_stream(p, std::move(pull));
    }
    // Stops pulling at once (the backend drops the callback before returning: the producer may be destroyed
    // afterwards). Not touched by stop_all_sfx / stop_all_speech.
    void stop_stream(uint32_t handle);
    void set_stream_volume(uint32_t handle, float volume, float ramp_seconds = 0.0f);  // wall-time ramp
    bool stream_open(uint32_t handle) const;
    size_t open_streams() const { return streams_.size(); }
    float stream_gain(uint32_t handle) const;  // current voice gain (0 if not open)

    // ---- Frame update (wall-clock seconds since the last call).
    void update(double dt_seconds);

    const std::vector<LogEntry>& log() const { return log_; }
    std::string log_text() const;
    void clear_log() { log_.clear(); }
    void set_log_limit(size_t limit);  // AudioConfig::log_limit; trims the log at once when it is over the new limit
    int64_t time_us() const { return now_us_; }

    uint32_t active_voices(Bus pool) const;
    size_t live_instances() const;
    const AudioDatabase& database() const { return *db_; }
    const AudioConfig& config() const { return cfg_; }
    bool cinematic() const { return cinematic_; }

    // Exposed for tests.
    float attenuation(float distance, float event_saturation) const;  // §1.7
    float pan_for(const Vec3& pos) const;

private:
    struct Instance {
        uint32_t handle = 0;
        uint32_t event = kNoEvent;
        uint8_t type = 0;  // 0 2D, 1 2D attached, 2 3D position, 3 3D attached
        uint64_t owner = 0;
        Vec3 pos{};
        bool audible = true;
        std::string language;
        int state = 1;
        int64_t delay_us = 0;
        int32_t cycle = 0;
        int32_t volume = 100, pan = 50, pitch = 100, predelay_ms = 0, postdelay_ms = 0;
        uint32_t voice = 0;
        int64_t voice_start_us = 0;
        bool voice_loop = false;
        float fade_in_seconds = 0.0f;
        float fade_in_level = 1.0f;
        bool fading = false;
        float fade_level = 1.0f;
        float fade_seconds = 0.0f;
        bool is3d() const { return type >= 2; }
    };
    struct Stream {
        uint32_t voice = 0;
        uint32_t event = kNoEvent;
        float level = 0.0f;  // ramp level 0..1 (x Volume_Percent)
        float target = 1.0f;
        float rate = 0.0f;   // level per second (0 = no ramp)
        bool internal_loop = false;
        std::string file;
    };
    struct MusicEntry {
        uint32_t event = kNoEvent;
        uint8_t mode = 0;
        bool resume_previous = false;
        uint32_t prev_event = kNoEvent;
        uint8_t prev_mode = 0;
        int state = 1;  // 1 start file, 2 playing, 3 ended
        uint32_t files_played = 0;
    };
    struct BusState {
        float level = 0.8f, target = 0.8f, rate = 0.0f;
    };

    const SfxEvent& ev(const Instance& i) const { return db_->sfx()[i.event]; }
    void reject(uint32_t event, std::string_view name, const SfxPlay& p, const char* reason);
    float distance_to_listener(const Vec3& p) const;
    void run_instance(Instance& in, std::vector<std::pair<uint32_t, SfxPlay>>& chains);
    bool start_stage(Instance& in, int stage);
    std::string pick_sample(Instance& in, int stage);
    bool get_voice(Instance& in);
    VoiceParams voice_params(const Instance& in) const;
    void kill(Instance& in, const char* reason);
    void update_sfx(int64_t dt_us);
    void start_music_file(MusicEntry& e);
    void close_stream(Stream& s);
    void update_music(int64_t dt_us);
    void update_speech();
    void push(LogEntry e);

    const AudioDatabase* db_;
    VoiceBackend* backend_;
    SimRng* rng_;
    AudioConfig cfg_;
    AudioMode mode_ = AudioMode::Land;
    Listener listener_;
    int64_t now_us_ = 0;
    int64_t frame_dt_us_ = 0;  // wall time of the current SFX update
    uint32_t next_handle_ = 1;
    uint32_t next_voice_ = 1;
    std::vector<Instance> instances_;  // active list: 2D first, then 3D nearest first
    struct SeqCounters {
        uint32_t primary = 0, pre = 0, post = 0;
    };
    std::map<uint32_t, SeqCounters> sequential_;  // event -> counters (session-wide)
    std::function<std::optional<ObjectAudio>(uint64_t)> object_query_;
    std::function<bool(const Vec3&)> position_fogged_;
    bool allow_localized_ = true, allow_unit_response_vo_ = true, allow_hud_vo_ = true, allow_ambient_vo_ = true;
    bool cinematic_ = false, gui_only_ = false, modal_dialog_ = false, hud_vo_blocked_ = false, attract_ = false;
    float scale_3d_ = 1.0f;
    BusState buses_[4];
    // music
    MusicResolver music_resolver_;
    bool music_enabled_ = true, music_lock_ = false;
    uint8_t music_mode_ = 0, base_music_mode_ = music_mode::kGalactic;
    uint32_t peace_ticks_ = 0;
    std::vector<MusicEntry> music_queue_;
    std::map<uint32_t, uint32_t> music_file_counter_;  // event -> next file index (session-wide)
    Stream music_current_, music_previous_;
    // speech
    struct SpeechEntry {
        uint32_t event = kNoEvent;
        uint32_t ticket = 0;
    };
    std::deque<SpeechEntry> speech_queue_;
    uint32_t speech_voice_ = 0;
    uint32_t next_speech_ticket_ = 1;
    float saved_master_ = 0.8f;
    std::vector<std::function<void(const std::string&, bool)>> speech_done_;
    std::vector<std::function<void(const SpeechEvent&, const std::string&)>> speech_start_;
    // streams
    struct PcmStreamState {
        uint32_t handle = 0, voice = 0;
        StreamParams params;
        float level = 1.0f, target = 1.0f, rate = 0.0f;  // volume ramp
    };
    float stream_voice_gain(const PcmStreamState& s) const;
    bool ducking_active() const;
    std::vector<PcmStreamState> streams_;
    uint32_t next_stream_ = 1;
    std::vector<LogEntry> log_;
};

}  // namespace alamo::audio
