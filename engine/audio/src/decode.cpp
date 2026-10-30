#include "alamo/audio/decode.hpp"

#include <cstring>

#include "ma_include.hpp"

namespace alamo::audio {
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// fmt tag of a RIFF/WAVE file (0 if not found).
uint16_t wav_format_tag(std::span<const uint8_t> b) {
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const uint32_t size = rd32(b.data() + pos + 4);
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0 && pos + 10 <= b.size()) return rd16(b.data() + pos + 8);
        pos += 8 + static_cast<size_t>(size) + (size & 1u);
    }
    return 0;
}

struct Decoder {
    ma_decoder dec{};
    bool ok = false;
    ~Decoder() {
        if (ok) ma_decoder_uninit(&dec);
    }
};

bool open(std::span<const uint8_t> bytes, Decoder& d, std::string* err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);  // native channels/rate
    const ma_result r = ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &d.dec);
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("miniaudio: ") + ma_result_description(r);
        return false;
    }
    d.ok = true;
    return true;
}

}  // namespace

Container sniff_container(std::span<const uint8_t> b) {
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0)
        return Container::Wav;
    if (b.size() >= 3 && std::memcmp(b.data(), "ID3", 3) == 0) return Container::Mp3;
    if (b.size() >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return Container::Mp3;
    return Container::Unknown;
}

std::optional<AudioInfo> probe_audio(std::span<const uint8_t> bytes, std::string* err) {
    Decoder d;
    if (!open(bytes, d, err)) return std::nullopt;
    AudioInfo info;
    info.container = sniff_container(bytes);
    if (info.container == Container::Wav) info.wav_format_tag = wav_format_tag(bytes);
    ma_format fmt{};
    ma_uint32 ch = 0, rate = 0;
    if (ma_decoder_get_data_format(&d.dec, &fmt, &ch, &rate, nullptr, 0) != MA_SUCCESS) {
        if (err) *err = "miniaudio: no data format";
        return std::nullopt;
    }
    info.channels = ch;
    info.sample_rate = rate;
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&d.dec, &len) != MA_SUCCESS) len = 0;
    info.frames = len;
    return info;
}

bool decode_audio(std::span<const uint8_t> bytes, PcmBuffer& out, std::string* err) {
    out = PcmBuffer{};
    Decoder d;
    if (!open(bytes, d, err)) return false;
    ma_format fmt{};
    ma_uint32 ch = 0, rate = 0;
    if (ma_decoder_get_data_format(&d.dec, &fmt, &ch, &rate, nullptr, 0) != MA_SUCCESS || ch == 0) {
        if (err) *err = "miniaudio: no data format";
        return false;
    }
    out.channels = ch;
    out.sample_rate = rate;
    // Read in chunks: MP3 length needs a scan anyway, and WAV lengths are exact.
    constexpr ma_uint64 kChunk = 16384;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * ch);
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&d.dec, chunk.data(), kChunk, &got);
        out.samples.insert(out.samples.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got * ch));
        if (r != MA_SUCCESS || got < kChunk) break;
    }
    if (out.samples.empty()) {
        if (err) *err = "miniaudio: no frames decoded";
        return false;
    }
    return true;
}

}  // namespace alamo::audio
