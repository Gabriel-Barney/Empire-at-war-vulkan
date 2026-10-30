// Streaming PCM resampler (see alamo/audio/pcm_stream.hpp).
#include "alamo/audio/pcm_stream.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::audio {

StreamResampler::StreamResampler(PcmStreamFormat source, uint32_t device_rate, uint32_t device_channels, PcmPullFn pull)
    : src_(source), dst_rate_(device_rate == 0 ? 44100u : device_rate), dst_ch_(device_channels == 0 ? 2u : device_channels),
      pull_(std::move(pull)) {
    if (src_.sample_rate == 0) src_.sample_rate = dst_rate_;
    if (src_.channels == 0) src_.channels = 1;
    step_ = static_cast<double>(src_.sample_rate) / static_cast<double>(dst_rate_);
}

void StreamResampler::map(const float* in, float* out) const {
    const uint32_t sc = src_.channels, dc = dst_ch_;
    if (sc == dc) {
        std::copy(in, in + sc, out);
    } else if (sc == 1) {
        std::fill(out, out + dc, in[0]);
    } else if (dc == 1) {
        float s = 0.0f;
        for (uint32_t c = 0; c < sc; ++c) s += in[c];
        out[0] = s / static_cast<float>(sc);
    } else {
        const uint32_t n = std::min(sc, dc);
        std::copy(in, in + n, out);
        std::fill(out + n, out + dc, 0.0f);
    }
}

size_t StreamResampler::pull_more(size_t want) {
    if (want == 0 || !pull_) return 0;
    const size_t old = buf_.size();
    buf_.resize(old + want * src_.channels);
    const size_t got = std::min(want, pull_(buf_.data() + old, want));
    buf_.resize(old + got * src_.channels);
    pulled_ += got;
    return got;
}

size_t StreamResampler::read(float* out, size_t frames) {
    const uint32_t sc = src_.channels, dc = dst_ch_;
    size_t done = 0;
    if (src_.sample_rate == dst_rate_) {
        // 1:1: no lookahead, the producer's clock sees exactly the frames the device consumes.
        tmp_.resize(frames * sc);
        size_t got = 0;
        if (pull_) got = std::min(frames, pull_(tmp_.data(), frames));
        pulled_ += got;
        for (; done < got; ++done) map(tmp_.data() + done * sc, out + done * dc);
    } else {
        std::vector<float>& mixed = mix_;
        mixed.resize(sc);
        for (; done < frames; ++done) {
            const size_t i = static_cast<size_t>(pos_);
            size_t have = buf_.size() / sc;
            if (i + 1 >= have) {
                // Pull what the rest of this block needs (+1 frame of interpolation lookahead).
                const double last = pos_ + static_cast<double>(frames - done - 1) * step_;
                const size_t need = static_cast<size_t>(last) + 2;
                pull_more(need > have ? need - have : 1);
                have = buf_.size() / sc;
                if (i + 1 >= have) break;  // underrun: wait for data, keep the position
            }
            const float frac = static_cast<float>(pos_ - static_cast<double>(i));
            const float* f0 = buf_.data() + i * sc;
            const float* f1 = f0 + sc;
            for (uint32_t c = 0; c < sc; ++c) mixed[c] = f0[c] + (f1[c] - f0[c]) * frac;
            map(mixed.data(), out + done * dc);
            pos_ += step_;
        }
        // Drop fully consumed source frames.
        const size_t drop = std::min(static_cast<size_t>(pos_), buf_.size() / sc);
        if (drop > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * sc));
            pos_ -= static_cast<double>(drop);
        }
    }
    std::fill(out + done * dc, out + frames * dc, 0.0f);
    underrun_ += frames - done;
    return done;
}

}  // namespace alamo::audio
