// alamo_video - Bink movie playback (docs/PHASE4.md P3) through FFmpeg's Bink decoder.
//
// FFmpeg (approved by Gabriel 2026-10-27) is the pinned 8.1.3 Bink-only LGPL v2.1+ shared build from
// toolchain\9_ffmpeg_bink.cmd: libavcodec bink / binkaudio_rdft / binkaudio_dct decoders, libavformat bink
// demuxer, libavutil. The three DLLs (avutil-60, avcodec-62, avformat-62) are loaded at RUNTIME
// (LoadLibrary / dlopen), so nothing links against FFmpeg and the engine builds with any of the three
// compilers; the DLLs must sit next to the exe (CMake: alamo_video_copy_ffmpeg_dlls(<target>)) or in the
// folder given to set_ffmpeg_dir() / the build-time default (toolchain\ffmpeg-8.1.3\bin).
//
// Data flow: the whole .bik is read through a FileProvider (the VFS: movies live in Data\Art\Movies\Binked,
// inside MOVIES.MEG) and demuxed from memory. Video frames come out as RGBA8 (or raw YUV420P planes) in a
// CPU buffer for the renderer / UI to upload; audio comes out as interleaved float32 PCM for engine/audio.
//
// Movies are presentation only: nothing here feeds the simulation, so the determinism rules for sim code do
// not apply (decode output is still bit-identical run to run: FFmpeg's Bink decoders are integer/float C
// code built without asm, and the YUV->RGBA conversion is integer).
//
// Threading: one BinkDecoder / MoviePlayer is used from one thread at a time, except
// MoviePlayer::read_audio(), which the audio thread may call concurrently with everything else.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}

namespace alamo::video {

// ---- FFmpeg runtime ----------------------------------------------------------------------------------
// Folder the FFmpeg DLLs are loaded from when they are not next to the executable. Call before the first
// load (later calls have no effect once loaded). "" restores the build-time default.
void set_ffmpeg_dir(std::string dir_utf8);
// Loads the DLLs once (thread-safe). False + *err when missing or when a required export is absent.
bool ffmpeg_available(std::string* err = nullptr);
// "FFmpeg 8.1.3, avcodec 62.x.y, avformat 62.x.y, avutil 60.x.y, LGPL version 2.1 or later" ("" if not loaded).
std::string ffmpeg_version_string();
// The library's license string (avcodec_license()), e.g. "LGPL version 2.1 or later"; "" if not loaded.
std::string ffmpeg_license();

// ---- Decoder -----------------------------------------------------------------------------------------
enum class PixelFormat : uint8_t {
    Rgba8,    // one plane, stride = width * 4, R G B A bytes; A = 255 unless the movie has an alpha plane
    Yuv420p,  // raw decoder planes copied tightly: Y (w x h), U, V ((w+1)/2 x (h+1)/2), A (w x h) if alpha
};

struct VideoInfo {
    int width = 0;
    int height = 0;
    int fps_num = 0;            // frame rate = fps_num / fps_den (Bink stores it as a rational)
    int fps_den = 1;
    int64_t frame_count = 0;    // from the Bink header
    double duration_s = 0.0;    // frame_count / fps
    bool has_alpha = false;     // alpha plane in the output: the file has one (YUVA420P) and OpenOptions::alpha
    bool file_alpha = false;    // the file has an alpha plane (whatever OpenOptions::alpha says)
    uint32_t codec_tag = 0;     // 'BIKb'..'BIKk' version tag as stored (little-endian FourCC)
    int audio_tracks = 0;       // number of Bink audio tracks in the file
    int sample_rate = 0;        // selected audio track (0 = none / audio off)
    int channels = 0;
};

struct VideoFrame {
    int64_t index = -1;         // 0-based frame number (= pts in 1/fps units)
    double pts_s = 0.0;         // index * fps_den / fps_num
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Rgba8;
    bool full_range = false;    // YUV range the decoder reported (JPEG/full vs MPEG/limited); used for Rgba8
    std::vector<uint8_t> data;  // planes, tightly packed one after another
    std::array<size_t, 4> plane_offset{};  // byte offset of each plane in data
    std::array<int, 4> plane_stride{};     // bytes per row (0 = plane absent)
    int plane_count = 0;
};

struct OpenOptions {
    PixelFormat format = PixelFormat::Rgba8;
    int audio_track = 0;        // which Bink audio track to decode; -1 = no audio
    // Movies.xml `Alpha` (docs/re/STRATEGIC_GAPS.md §7.3 [H]: the original opens alpha movies with the alpha-plane flag
    // and copies BGRA with alpha; every other movie is drawn opaque). true: the file's alpha plane is kept (Rgba8 A =
    // the plane, straight - not premultiplied; Yuv420p gets the A plane); false: the plane is ignored (A = 255, no A
    // plane, VideoInfo::has_alpha false). Default true = whatever the file has.
    bool alpha = true;
};

// Movies.xml <Movie> fields the players need (STRATEGIC_GAPS §7.3): parse_movies (movie_db.hpp, built with engine/data).
struct MovieEntry {
    std::string name;            // Name attribute
    std::string file;            // Movie_File (game-relative path, "" = none)
    bool alpha = false;          // Alpha: decode the alpha plane, draw with straight alpha (PrimAlpha) - else opaque
    bool cannot_skip = false;    // Cannot_Skip
    bool loop = false;           // Loop
    int commandbar_offset[2] = {0, 0};  // Commandbar_Offset
};
// OpenOptions for a Movies.xml entry (alpha = entry.alpha).
inline OpenOptions open_options_for(const MovieEntry& m, PixelFormat format = PixelFormat::Rgba8, int audio_track = 0) {
    OpenOptions o;
    o.format = format;
    o.audio_track = audio_track;
    o.alpha = m.alpha;
    return o;
}

class BinkDecoder {
public:
    // Reads `path` (game-relative, e.g. "Data\\Art\\Movies\\Binked\\Intro.bik") through `files`.
    static std::unique_ptr<BinkDecoder> open(const alamo::FileProvider& files, std::string_view path,
                                             const OpenOptions& opt = {}, std::string* err = nullptr);
    // Takes the whole file in memory.
    static std::unique_ptr<BinkDecoder> open_memory(std::vector<uint8_t> bytes, const OpenOptions& opt = {},
                                                    std::string* err = nullptr);
    ~BinkDecoder();
    BinkDecoder(const BinkDecoder&) = delete;
    BinkDecoder& operator=(const BinkDecoder&) = delete;

    const VideoInfo& info() const;
    // Decodes the next video frame into `out` (buffers reused). Audio packets met on the way are decoded and
    // queued (take_audio). False at the end of the movie or on a decode error (see error()).
    bool next_frame(VideoFrame& out);
    // Appends all queued audio as interleaved float32 (info().channels per sample frame) and returns the
    // number of sample frames appended.
    size_t take_audio(std::vector<float>& interleaved);
    // Back to frame 0 (reopens the demuxer on the in-memory file); audio queue cleared.
    bool rewind();
    bool at_end() const;
    const std::string& error() const;

    struct Impl;

private:
    explicit BinkDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// YUV(A)420P -> RGBA8, BT.601 integer math. full_range: JPEG range (Y 0..255) instead of 16..235.
// Exposed for tests and for callers that keep Yuv420p frames and convert later.
void yuv420_to_rgba(const uint8_t* y, int y_stride, const uint8_t* u, int u_stride, const uint8_t* v, int v_stride,
                    const uint8_t* a, int a_stride, int width, int height, bool full_range, uint8_t* rgba,
                    int rgba_stride);

// ---- Player + clock -------------------------------------------------------------------------------------
// Drives a BinkDecoder from a playback clock. The host calls update(dt) once per rendered frame and uploads
// current_frame() when frame_serial() changed; the audio thread pulls PCM with read_audio().
//
// Clock: Wall (default) advances by update(dt). Audio uses the audio device's consumption as the master clock
// (time = sample frames handed out by read_audio / sample_rate), which keeps lip-sync when the device runs
// slightly fast or slow; it falls back to dt while no audio has been consumed yet and after the audio track
// ended. Frames are never decoded ahead of the clock; when the clock jumps several frames, the intermediate
// frames are decoded and dropped (Bink is inter-coded, so none can be skipped).
class MoviePlayer {
public:
    enum class Clock : uint8_t { Wall, Audio };
    explicit MoviePlayer(std::unique_ptr<BinkDecoder> decoder, Clock clock = Clock::Wall);
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    // Advances the clock (seconds) and decodes up to the frame due at the new time. Returns true when the
    // current frame changed. No-op while paused or finished.
    bool update(double dt_s);
    const VideoFrame* current_frame() const;  // nullptr before the first frame
    uint64_t frame_serial() const;            // increments on every new current frame
    double time_s() const;                    // playback clock
    bool finished() const;                    // last frame shown and its display time elapsed (never with loop)
    void set_paused(bool paused);
    bool paused() const;
    void set_loop(bool loop);                 // restart at the end (audio continues seamlessly)
    void stop();                              // finished() becomes true, audio queue cleared
    const VideoInfo& info() const;
    const BinkDecoder& decoder() const;
    int64_t frames_dropped() const;           // decoded but never current (clock ran ahead)
    // OI host (P3 video item 3): Audio clock only - the output device's buffering (seconds, >= 0). A sample handed
    // out by read_audio is heard this much later, so the clock is (frames handed out / rate) - latency, never
    // running backwards (the first frame is held until the first samples are audible).
    void set_audio_latency(double seconds) { latency_s_ = seconds > 0.0 ? seconds : 0.0; }
    double audio_latency() const { return latency_s_; }

    // Audio thread: copies up to `frames` interleaved float32 sample frames (info().channels each) into
    // `out`, returns the number copied (0 when paused, none queued, or no audio). Thread-safe.
    size_t read_audio(float* out, size_t frames);
    size_t audio_queued_frames() const;       // sample frames waiting (thread-safe)

private:
    void pump_audio_();
    bool decode_one_();
    void fill_();
    double due_s_(int64_t global_index) const;

    std::unique_ptr<BinkDecoder> dec_;
    Clock clock_;
    int channels_ = 0;                 // copy of info().channels (read by the audio thread)
    VideoFrame cur_;
    bool have_frame_ = false;
    std::deque<VideoFrame> pending_;   // decoded, not yet due (pts_s on the player clock)
    std::vector<VideoFrame> pool_;     // recycled frame buffers
    uint64_t serial_ = 0;
    double time_ = 0.0;
    double waited_s_ = 0.0;            // Audio clock: time spent waiting for the device to start
    bool audio_fallback_ = false;      // Audio clock: device never pulled, run on dt
    double latency_s_ = 0.0;           // Audio clock: output device latency (OI host)
    bool paused_ = false;
    bool loop_ = false;
    bool finished_ = false;
    bool decoder_done_ = false;
    int64_t dropped_ = 0;
    int64_t pass_base_ = 0;            // frames of earlier loop passes (player frame times keep increasing)
    int64_t pass_frames_ = 0;          // frames decoded in the current pass
    int64_t last_global_ = -1;         // player frame number of the last decoded frame

    mutable std::mutex audio_mx_;
    std::vector<float> audio_q_;       // interleaved, consumed from audio_head_
    size_t audio_head_ = 0;            // in floats
    std::vector<float> audio_tmp_;
    std::atomic<int64_t> audio_consumed_{0};  // sample frames handed to the device
    std::atomic<bool> audio_paused_{false};
};

}  // namespace alamo::video
