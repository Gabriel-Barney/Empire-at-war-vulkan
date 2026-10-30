// Streaming PCM voices: a producer (movie decoder, voice chat, procedural audio) hands the mixer a pull
// callback instead of a file. Used for Bink movie sound: alamo::video::MoviePlayer::read_audio has exactly
// the PcmPullFn shape, so the host binds it with a lambda and engine/audio never links engine/video.
//
// Clock contract: the backend pulls only what its device consumes (MoviePlayer's Audio clock follows the
// frames handed out). With equal source and device rates frames are pulled 1:1; with rate conversion the
// linear resampler holds at most one source frame of lookahead. The miniaudio backend adds the node graph's
// read-ahead: bounded by its processing chunk (measured 100..223 source frames at 22.05 kHz, <= ~10 ms),
// never growing: the pull rate follows consumption.
// Underrun: when the callback returns fewer frames than asked, the rest of that device block is silence and
// the stream position does not advance past the data received (the stream stays open; nothing is skipped).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace alamo::audio {

// Fills up to `frames` interleaved float32 sample frames into `out`, returns the number written. Called on
// the mixing thread (miniaudio's device thread; the null backend's owner thread), so it must be thread-safe
// with respect to the producer. It must stay callable until the stream is stopped.
using PcmPullFn = std::function<size_t(float* out, size_t frames)>;

struct PcmStreamFormat {
    uint32_t sample_rate = 0;  // source rate (Hz)
    uint32_t channels = 0;     // source channels (interleaved)
};

// Stateful linear-interpolation resampler + channel mapper from a pull source to the device format.
// Channel mapping: equal -> copy; mono source -> every device channel; mono device -> average; otherwise
// the common channels are copied and extra device channels are silent.
class StreamResampler {
public:
    StreamResampler(PcmStreamFormat source, uint32_t device_rate, uint32_t device_channels, PcmPullFn pull);

    // Writes exactly `frames` device frames (interleaved, device_channels each) into `out`; returns the
    // number that carry source data (the rest is underrun silence).
    size_t read(float* out, size_t frames);

    uint64_t source_frames_pulled() const { return pulled_; }
    uint64_t underrun_frames() const { return underrun_; }  // device frames filled with silence
    const PcmStreamFormat& source() const { return src_; }
    uint32_t device_rate() const { return dst_rate_; }
    uint32_t device_channels() const { return dst_ch_; }

private:
    void map(const float* in, float* out) const;
    size_t pull_more(size_t want);  // appends up to `want` source frames to buf_, returns frames received

    PcmStreamFormat src_;
    uint32_t dst_rate_, dst_ch_;
    PcmPullFn pull_;
    double step_;               // source frames per device frame
    double pos_ = 0.0;          // read position in buf_ (source frames)
    std::vector<float> buf_;    // buffered source frames (interleaved)
    std::vector<float> tmp_, mix_;
    uint64_t pulled_ = 0, underrun_ = 0;
};

}  // namespace alamo::audio
