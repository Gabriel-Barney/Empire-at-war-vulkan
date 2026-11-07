#include "alamo/audio/miniaudio_backend.hpp"

#include <map>

#include "alamo/audio/decode.hpp"
#include "alamo/core/hash.hpp"
#include "ma_include.hpp"

namespace alamo::audio {

namespace {

// Stream voice data source: presents the device format to miniaudio (no miniaudio resampling); reads run
// on the device thread and pull through StreamResampler, which always fills the block (silence on underrun)
// so the sound never reaches an end.
struct PullSource {             // standard layout: miniaudio hands back &base
    ma_data_source_base base;  // must be first
    StreamResampler* resampler;
    bool ok;
};

ma_result pull_read(ma_data_source* ds, void* out, ma_uint64 frames, ma_uint64* read) {
    PullSource* s = static_cast<PullSource*>(ds);
    s->resampler->read(static_cast<float*>(out), static_cast<size_t>(frames));
    if (read) *read = frames;
    return MA_SUCCESS;
}
ma_result pull_seek(ma_data_source*, ma_uint64) { return MA_NOT_IMPLEMENTED; }
ma_result pull_format(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map,
                      size_t map_cap) {
    const PullSource* s = static_cast<const PullSource*>(ds);
    if (format) *format = ma_format_f32;
    if (channels) *channels = s->resampler->device_channels();
    if (rate) *rate = s->resampler->device_rate();
    if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, map_cap, s->resampler->device_channels());
    return MA_SUCCESS;
}
ma_result pull_cursor(ma_data_source*, ma_uint64* cursor) {
    if (cursor) *cursor = 0;
    return MA_NOT_IMPLEMENTED;
}
ma_result pull_length(ma_data_source*, ma_uint64* length) {
    if (length) *length = 0;
    return MA_NOT_IMPLEMENTED;
}

ma_data_source_vtable make_pull_vtable() {
    ma_data_source_vtable v{};
    v.onRead = pull_read;
    v.onSeek = pull_seek;
    v.onGetDataFormat = pull_format;
    v.onGetCursor = pull_cursor;
    v.onGetLength = pull_length;
    return v;
}
const ma_data_source_vtable g_pull_vtable = make_pull_vtable();

struct Voice {
    ma_sound sound{};
    bool sound_ok = false;
    std::unique_ptr<StreamResampler> resampler;  // stream voices
    std::unique_ptr<PullSource> pull;
    // SFX: cached PCM + a ref data source over it.
    std::shared_ptr<const PcmBuffer> pcm;
    ma_audio_buffer_ref ref{};
    bool ref_ok = false;
    // Music/speech: owned bytes + streaming decoder.
    std::vector<uint8_t> bytes;
    ma_decoder dec{};
    bool dec_ok = false;

    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    ~Voice() {
        if (sound_ok) ma_sound_uninit(&sound);  // detached from the graph: the device thread no longer reads it
        if (pull && pull->ok) ma_data_source_uninit(&pull->base);
        if (dec_ok) ma_decoder_uninit(&dec);
        if (ref_ok) ma_audio_buffer_ref_uninit(&ref);
    }
};

}  // namespace

struct MiniaudioBackend::Impl {
    const SampleSource* samples = nullptr;
    ma_engine engine{};
    bool engine_ok = false;
    std::map<uint32_t, std::unique_ptr<Voice>> voices;
    std::map<std::string, std::shared_ptr<const PcmBuffer>> cache;  // normalize_name(file)
    std::string error;

    ~Impl() {
        voices.clear();
        if (engine_ok) ma_engine_uninit(&engine);
    }

    void apply(Voice& v, const VoiceParams& p) {
        ma_sound_set_volume(&v.sound, p.gain);
        ma_sound_set_pan(&v.sound, p.pan);
        ma_sound_set_pitch(&v.sound, p.pitch > 0.0f ? p.pitch : 1.0f);
        ma_sound_set_looping(&v.sound, p.loop ? MA_TRUE : MA_FALSE);
    }
};

MiniaudioBackend::MiniaudioBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MiniaudioBackend::~MiniaudioBackend() = default;

std::unique_ptr<MiniaudioBackend> MiniaudioBackend::create(const SampleSource& samples, const MiniaudioConfig& cfg,
                                                           std::string* err) {
    auto impl = std::make_unique<Impl>();
    impl->samples = &samples;
    ma_engine_config ec = ma_engine_config_init();
    ec.noDevice = cfg.no_device ? MA_TRUE : MA_FALSE;
    ec.channels = cfg.channels;
    ec.sampleRate = cfg.sample_rate;
    if (cfg.no_device && ec.sampleRate == 0) ec.sampleRate = 44100;
    const ma_result r = ma_engine_init(&ec, &impl->engine);
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("ma_engine_init: ") + ma_result_description(r);
        return nullptr;
    }
    impl->engine_ok = true;
    return std::unique_ptr<MiniaudioBackend>(new MiniaudioBackend(std::move(impl)));
}

bool MiniaudioBackend::start(uint32_t voice, const SampleRef& sample, const VoiceParams& p) {
    Impl& im = *impl_;
    im.voices.erase(voice);  // restart = fresh sound
    auto v = std::make_unique<Voice>();
    ma_data_source* ds = nullptr;
    if (sample.bus == Bus::Sfx2D || sample.bus == Bus::Sfx3D) {
        const std::string key = normalize_name(sample.file);
        auto it = im.cache.find(key);
        if (it == im.cache.end()) {
            std::vector<uint8_t> bytes;
            if (!im.samples->read(sample, bytes)) {
                im.error = "missing sample " + sample.file;
                return false;
            }
            auto pcm = std::make_shared<PcmBuffer>();
            std::string err;
            if (!decode_audio(bytes, *pcm, &err)) {
                im.error = sample.file + ": " + err;
                return false;
            }
            it = im.cache.emplace(key, std::move(pcm)).first;
        }
        v->pcm = it->second;
        if (ma_audio_buffer_ref_init(ma_format_f32, v->pcm->channels, v->pcm->samples.data(), v->pcm->frames(),
                                     &v->ref) != MA_SUCCESS) {
            im.error = "ma_audio_buffer_ref_init failed";
            return false;
        }
        v->ref.sampleRate = v->pcm->sample_rate;
        v->ref_ok = true;
        ds = &v->ref;
    } else {
        if (!im.samples->read(sample, v->bytes)) {
            im.error = "missing stream " + sample.file;
            return false;
        }
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (ma_decoder_init_memory(v->bytes.data(), v->bytes.size(), &dc, &v->dec) != MA_SUCCESS) {
            im.error = "cannot decode " + sample.file;
            return false;
        }
        v->dec_ok = true;
        ds = &v->dec;
    }
    if (ma_sound_init_from_data_source(&im.engine, ds, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &v->sound) !=
        MA_SUCCESS) {
        im.error = "ma_sound_init_from_data_source failed";
        return false;
    }
    v->sound_ok = true;
    im.apply(*v, p);
    ma_sound_start(&v->sound);
    im.voices.emplace(voice, std::move(v));
    return true;
}

bool MiniaudioBackend::start_stream(uint32_t voice, const PcmStreamFormat& format, PcmPullFn pull,
                                    const VoiceParams& p) {
    Impl& im = *impl_;
    im.voices.erase(voice);
    auto v = std::make_unique<Voice>();
    v->resampler = std::make_unique<StreamResampler>(format, ma_engine_get_sample_rate(&im.engine),
                                                     ma_engine_get_channels(&im.engine), std::move(pull));
    v->pull = std::make_unique<PullSource>();
    v->pull->resampler = v->resampler.get();
    v->pull->ok = false;
    ma_data_source_config dc = ma_data_source_config_init();
    dc.vtable = &g_pull_vtable;
    if (ma_data_source_init(&dc, &v->pull->base) != MA_SUCCESS) {
        im.error = "ma_data_source_init failed";
        return false;
    }
    v->pull->ok = true;
    if (ma_sound_init_from_data_source(&im.engine, &v->pull->base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                       &v->sound) != MA_SUCCESS) {
        im.error = "ma_sound_init_from_data_source failed (stream)";
        return false;
    }
    v->sound_ok = true;
    im.apply(*v, p);
    ma_sound_start(&v->sound);
    im.voices.emplace(voice, std::move(v));
    return true;
}

void MiniaudioBackend::set(uint32_t voice, const VoiceParams& p) {
    auto it = impl_->voices.find(voice);
    if (it != impl_->voices.end()) impl_->apply(*it->second, p);
}

void MiniaudioBackend::stop(uint32_t voice) { impl_->voices.erase(voice); }

bool MiniaudioBackend::playing(uint32_t voice) const {
    auto it = impl_->voices.find(voice);
    if (it == impl_->voices.end()) return false;
    Voice& v = *it->second;
    return ma_sound_is_playing(&v.sound) && !ma_sound_at_end(&v.sound);
}

void MiniaudioBackend::advance(double) {}

uint64_t MiniaudioBackend::read_mix(float* out, uint64_t frames) {
    ma_uint64 got = 0;
    if (ma_engine_read_pcm_frames(&impl_->engine, out, frames, &got) != MA_SUCCESS) return 0;
    return got;
}

double MiniaudioBackend::output_latency_seconds() const {  // OI host
    ma_device* dev = ma_engine_get_device(&impl_->engine);
    if (!dev || dev->playback.internalSampleRate == 0) return 0.0;
    return static_cast<double>(dev->playback.internalPeriodSizeInFrames) * static_cast<double>(dev->playback.internalPeriods) /
           static_cast<double>(dev->playback.internalSampleRate);
}

uint32_t MiniaudioBackend::sample_rate() const { return ma_engine_get_sample_rate(&impl_->engine); }
size_t MiniaudioBackend::cached_samples() const { return impl_->cache.size(); }

}  // namespace alamo::audio
