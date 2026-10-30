// Decoding of the shipped audio formats through miniaudio's built-in decoders: RIFF WAV (dr_wav: PCM,
// IEEE float, MS/IMA ADPCM, A-law, mu-law) and MP3 (dr_mp3). The shipped corpus is 16-bit PCM WAV
// (SFX, 44.1 kHz mono) and MP3 (music, speech) - docs/assets/CATALOG.md.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alamo::audio {

enum class Container : uint8_t { Unknown, Wav, Mp3 };

struct AudioInfo {
    Container container = Container::Unknown;
    uint16_t wav_format_tag = 0;  // WAVE fmt tag (1 = PCM, 2 = MS ADPCM, 0x11 = IMA ADPCM, ...), 0 for MP3
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint64_t frames = 0;  // length in PCM frames (MP3: exact, by scanning)
    double seconds() const { return sample_rate ? static_cast<double>(frames) / sample_rate : 0.0; }
};

// Header sniffing only (RIFF/WAVE, ID3 or MPEG frame sync).
Container sniff_container(std::span<const uint8_t> bytes);

// Opens the data with miniaudio and reads format + length. nullopt if it cannot be decoded.
std::optional<AudioInfo> probe_audio(std::span<const uint8_t> bytes, std::string* err = nullptr);

struct PcmBuffer {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<float> samples;  // interleaved f32
    uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

// Full decode to interleaved f32 at the file's own rate and channel count.
bool decode_audio(std::span<const uint8_t> bytes, PcmBuffer& out, std::string* err = nullptr);

}  // namespace alamo::audio
