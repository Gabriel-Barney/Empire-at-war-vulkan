// VoiceBackend on miniaudio's high-level engine (ma_engine / ma_sound). Spatialisation, gain and pan
// are computed by AudioSystem (so the null backend and this one get identical parameters); every
// sound is created with MA_SOUND_FLAG_NO_SPATIALIZATION and only volume / pan / pitch / looping are
// set. SFX samples are decoded once to f32 and cached by name; music and speech stream from memory.
// miniaudio mixes on its own device thread; all calls here come from the AudioSystem owner thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "alamo/audio/audio_system.hpp"
#include "alamo/audio/sample_source.hpp"

namespace alamo::audio {

struct MiniaudioConfig {
    bool no_device = false;  // true: no output device; pull the mix with read_mix() (tests, capture)
    uint32_t channels = 2;
    uint32_t sample_rate = 44100;  // 0 = device default (no_device needs a rate)
};

class MiniaudioBackend final : public VoiceBackend {
public:
    // `samples` must outlive the backend. nullptr + *err when the engine/device cannot start.
    static std::unique_ptr<MiniaudioBackend> create(const SampleSource& samples, const MiniaudioConfig& cfg,
                                                    std::string* err = nullptr);
    ~MiniaudioBackend() override;

    bool start(uint32_t voice, const SampleRef& sample, const VoiceParams& p) override;
    void set(uint32_t voice, const VoiceParams& p) override;
    void stop(uint32_t voice) override;
    bool playing(uint32_t voice) const override;
    void advance(double seconds) override;
    // Stream voices: StreamResampler converts to the engine rate/channels; pulls run on the device thread
    // (in no_device mode: inside read_mix). stop() returns after the sound left the node graph.
    bool start_stream(uint32_t voice, const PcmStreamFormat& format, PcmPullFn pull, const VoiceParams& p) override;
    // OI host: the playback device's internal buffer (internal period size x periods / internal rate); 0 in
    // no_device mode. Excludes the node graph's own read-ahead (pcm_stream.hpp: <= ~10 ms).
    double output_latency_seconds() const override;

    // no_device mode: mixes `frames` frames of interleaved f32 into `out`; returns frames produced.
    uint64_t read_mix(float* out, uint64_t frames);
    uint32_t sample_rate() const;
    size_t cached_samples() const;

    struct Impl;

private:
    explicit MiniaudioBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace alamo::audio
