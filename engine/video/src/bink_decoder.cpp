// BinkDecoder: libavformat bink demuxer over an in-memory AVIOContext + libavcodec bink / binkaudio decoders.
#include <algorithm>
#include <cstring>
#include <limits>

#include "alamo/core/file_provider.hpp"
#include "alamo/video/video.hpp"
#include "ffmpeg_api.hpp"

namespace alamo::video {

using detail::Api;
using detail::ff_error;

struct BinkDecoder::Impl {
    const Api* ff = nullptr;
    std::vector<uint8_t> bytes;
    int64_t pos = 0;
    OpenOptions opt;
    VideoInfo info;
    std::string err;

    AVIOContext* io = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* aframe = nullptr;
    int vstream = -1;
    int astream = -1;
    int64_t decoded = 0;  // video frames out of the decoder this pass
    bool demux_eof = false;
    bool flushed = false;
    bool end = false;

    std::vector<float> audio;  // queued interleaved PCM
    int64_t audio_total = 0;

    ~Impl() { close(); }

    void close() {
        if (!ff) return;
        if (pkt) ff->av_packet_free(&pkt);
        if (frame) ff->av_frame_free(&frame);
        if (aframe) ff->av_frame_free(&aframe);
        if (vctx) ff->avcodec_free_context(&vctx);
        if (actx) ff->avcodec_free_context(&actx);
        if (fmt) ff->avformat_close_input(&fmt);  // custom IO: pb is ours
        if (io) {
            ff->av_free(io->buffer);
            ff->avio_context_free(&io);
        }
        vstream = astream = -1;
    }

    static int read_cb(void* opaque, uint8_t* buf, int size) {
        auto* self = static_cast<Impl*>(opaque);
        const int64_t left = static_cast<int64_t>(self->bytes.size()) - self->pos;
        if (left <= 0) return AVERROR_EOF;
        const int n = static_cast<int>(std::min<int64_t>(left, size));
        std::memcpy(buf, self->bytes.data() + self->pos, static_cast<size_t>(n));
        self->pos += n;
        return n;
    }

    static int64_t seek_cb(void* opaque, int64_t offset, int whence) {
        auto* self = static_cast<Impl*>(opaque);
        const int64_t size = static_cast<int64_t>(self->bytes.size());
        if (whence & AVSEEK_SIZE) return size;
        int64_t np = 0;
        switch (whence & ~AVSEEK_FORCE) {
            case SEEK_SET: np = offset; break;
            case SEEK_CUR: np = self->pos + offset; break;
            case SEEK_END: np = size + offset; break;
            default: return -1;
        }
        if (np < 0 || np > size) return -1;
        self->pos = np;
        return np;
    }

    bool fail(std::string msg) {
        err = std::move(msg);
        return false;
    }

    AVCodecContext* open_codec(AVStream* st) {
        const AVCodec* codec = ff->avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            err = "no FFmpeg decoder for codec id " + std::to_string(static_cast<int>(st->codecpar->codec_id));
            return nullptr;
        }
        AVCodecContext* c = ff->avcodec_alloc_context3(codec);
        if (!c) {
            err = "avcodec_alloc_context3 failed";
            return nullptr;
        }
        int r = ff->avcodec_parameters_to_context(c, st->codecpar);
        if (r >= 0) {
            c->pkt_timebase = st->time_base;
            c->thread_count = 1;  // decode on the caller's thread
            r = ff->avcodec_open2(c, codec, nullptr);
        }
        if (r < 0) {
            err = "avcodec_open2: " + ff_error(r);
            ff->avcodec_free_context(&c);
            return nullptr;
        }
        return c;
    }

    // (Re)opens the demuxer and decoders on `bytes` from offset 0.
    bool open() {
        close();
        pos = 0;
        decoded = 0;
        demux_eof = flushed = end = false;
        constexpr int kIoBuf = 64 * 1024;
        auto* buf = static_cast<unsigned char*>(ff->av_malloc(kIoBuf));
        if (!buf) return fail("av_malloc failed");
        io = ff->avio_alloc_context(buf, kIoBuf, 0, this, &Impl::read_cb, nullptr, &Impl::seek_cb);
        if (!io) {
            ff->av_free(buf);
            return fail("avio_alloc_context failed");
        }
        fmt = ff->avformat_alloc_context();
        if (!fmt) return fail("avformat_alloc_context failed");
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        const AVInputFormat* bink = ff->av_find_input_format("bink");
        if (!bink) return fail("FFmpeg build has no bink demuxer");
        const int r = ff->avformat_open_input(&fmt, nullptr, bink, nullptr);
        if (r < 0) {
            fmt = nullptr;  // freed by avformat_open_input on failure
            return fail("not a Bink file (" + ff_error(r) + ")");
        }
        int audio_seen = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            st->discard = AVDISCARD_ALL;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vstream < 0) {
                vstream = static_cast<int>(i);
                st->discard = AVDISCARD_DEFAULT;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (audio_seen == opt.audio_track) {
                    astream = static_cast<int>(i);
                    st->discard = AVDISCARD_DEFAULT;
                }
                ++audio_seen;
            }
        }
        if (vstream < 0) return fail("Bink file has no video stream");
        AVStream* vs = fmt->streams[vstream];
        vctx = open_codec(vs);
        if (!vctx) return false;
        if (astream >= 0) {
            actx = open_codec(fmt->streams[astream]);
            if (!actx) return false;
        }
        pkt = ff->av_packet_alloc();
        frame = ff->av_frame_alloc();
        aframe = ff->av_frame_alloc();
        if (!pkt || !frame || !aframe) return fail("FFmpeg allocation failed");

        info = VideoInfo{};
        info.width = vs->codecpar->width;
        info.height = vs->codecpar->height;
        // The demuxer sets the time base to 1/fps (fps_den / fps_num from the header).
        info.fps_num = vs->time_base.den;
        info.fps_den = vs->time_base.num;
        if (info.fps_num <= 0 || info.fps_den <= 0) return fail("Bink file has an invalid frame rate");
        info.frame_count = vs->nb_frames > 0 ? vs->nb_frames : vs->duration;
        info.duration_s =
            static_cast<double>(info.frame_count) * info.fps_den / static_cast<double>(info.fps_num);
        info.file_alpha = vctx->pix_fmt == AV_PIX_FMT_YUVA420P;
        info.has_alpha = info.file_alpha && opt.alpha;  // Movies.xml Alpha (STRATEGIC_GAPS §7.3)
        info.codec_tag = vs->codecpar->codec_tag;
        info.audio_tracks = audio_seen;
        if (actx) {
            info.sample_rate = actx->sample_rate;
            info.channels = actx->ch_layout.nb_channels;
        }
        return true;
    }

    void queue_audio(const AVFrame* f) {
        const int ch = f->ch_layout.nb_channels;
        const int n = f->nb_samples;
        if (ch <= 0 || n <= 0 || ch != info.channels) return;
        const size_t base = audio.size();
        audio.resize(base + static_cast<size_t>(n) * static_cast<size_t>(ch));
        float* out = audio.data() + base;
        switch (static_cast<AVSampleFormat>(f->format)) {
            case AV_SAMPLE_FMT_FLTP:
                for (int c = 0; c < ch; ++c) {
                    const auto* src = reinterpret_cast<const float*>(f->extended_data[c]);
                    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i) * static_cast<size_t>(ch) + static_cast<size_t>(c)] = src[i];
                }
                break;
            case AV_SAMPLE_FMT_FLT:
                std::memcpy(out, f->extended_data[0], static_cast<size_t>(n) * static_cast<size_t>(ch) * sizeof(float));
                break;
            case AV_SAMPLE_FMT_S16P:
                for (int c = 0; c < ch; ++c) {
                    const auto* src = reinterpret_cast<const int16_t*>(f->extended_data[c]);
                    for (int i = 0; i < n; ++i)
                        out[static_cast<size_t>(i) * static_cast<size_t>(ch) + static_cast<size_t>(c)] = static_cast<float>(src[i]) / 32768.0f;
                }
                break;
            case AV_SAMPLE_FMT_S16: {
                const auto* src = reinterpret_cast<const int16_t*>(f->extended_data[0]);
                for (size_t i = 0; i < static_cast<size_t>(n) * static_cast<size_t>(ch); ++i)
                    out[i] = static_cast<float>(src[i]) / 32768.0f;
                break;
            }
            default:
                audio.resize(base);  // unexpected format: drop
                return;
        }
        audio_total += n;
    }

    void decode_audio_packet() {
        if (ff->avcodec_send_packet(actx, pkt) < 0) return;  // a bad audio packet is skipped, video goes on
        while (ff->avcodec_receive_frame(actx, aframe) >= 0) {
            queue_audio(aframe);
        }
    }

    void store_frame(VideoFrame& out) {
        const AVFrame* f = frame;
        const int w = f->width, h = f->height;
        int64_t idx = f->best_effort_timestamp;
        if (idx == AV_NOPTS_VALUE || idx < 0) idx = decoded;
        ++decoded;
        out.index = idx;
        out.pts_s = static_cast<double>(idx) * info.fps_den / static_cast<double>(info.fps_num);
        out.width = w;
        out.height = h;
        out.format = opt.format;
        out.full_range = f->color_range == AVCOL_RANGE_JPEG;
        out.plane_offset = {};
        out.plane_stride = {};
        const bool alpha = f->format == AV_PIX_FMT_YUVA420P && opt.alpha;  // an ignored plane: A = 255 / no A plane
        if (opt.format == PixelFormat::Rgba8) {
            out.plane_count = 1;
            out.plane_stride[0] = w * 4;
            out.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            yuv420_to_rgba(f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2],
                           alpha ? f->data[3] : nullptr, alpha ? f->linesize[3] : 0, w, h,
                           out.full_range, out.data.data(), w * 4);
            return;
        }
        const int cw = (w + 1) / 2, chh = (h + 1) / 2;
        const int pw[4] = {w, cw, cw, w};
        const int ph[4] = {h, chh, chh, h};
        out.plane_count = alpha ? 4 : 3;
        size_t total = 0;
        for (int p = 0; p < out.plane_count; ++p) {
            out.plane_offset[static_cast<size_t>(p)] = total;
            out.plane_stride[static_cast<size_t>(p)] = pw[p];
            total += static_cast<size_t>(pw[p]) * static_cast<size_t>(ph[p]);
        }
        out.data.resize(total);
        for (int p = 0; p < out.plane_count; ++p) {
            for (int row = 0; row < ph[p]; ++row) {
                std::memcpy(out.data.data() + out.plane_offset[static_cast<size_t>(p)] +
                                static_cast<size_t>(row) * static_cast<size_t>(pw[p]),
                            f->data[p] + static_cast<std::ptrdiff_t>(row) * f->linesize[p], static_cast<size_t>(pw[p]));
            }
        }
    }

    bool next_frame(VideoFrame& out) {
        if (end) return false;
        for (;;) {
            const int r = ff->avcodec_receive_frame(vctx, frame);
            if (r >= 0) {
                store_frame(out);
                return true;
            }
            if (r == AVERROR_EOF) {
                end = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) {
                end = true;
                return fail("video decode: " + ff_error(r));
            }
            if (demux_eof) {
                if (flushed) {
                    end = true;
                    return false;
                }
                flushed = true;
                ff->avcodec_send_packet(vctx, nullptr);  // drain
                continue;
            }
            const int rr = ff->av_read_frame(fmt, pkt);
            if (rr < 0) {
                demux_eof = true;
                if (rr != AVERROR_EOF) err = "demux: " + ff_error(rr);  // truncated file: decode what we have
                continue;
            }
            if (pkt->stream_index == vstream) {
                const int s = ff->avcodec_send_packet(vctx, pkt);
                ff->av_packet_unref(pkt);
                if (s < 0 && s != AVERROR(EAGAIN)) {
                    end = true;
                    return fail("video decode: " + ff_error(s));
                }
            } else {
                if (pkt->stream_index == astream && actx) decode_audio_packet();
                ff->av_packet_unref(pkt);
            }
        }
    }
};

BinkDecoder::BinkDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BinkDecoder::~BinkDecoder() = default;

std::unique_ptr<BinkDecoder> BinkDecoder::open(const alamo::FileProvider& files, std::string_view path,
                                               const OpenOptions& opt, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) {
        if (err) *err = "movie not found: " + std::string(path);
        return nullptr;
    }
    return open_memory(std::move(bytes), opt, err);
}

std::unique_ptr<BinkDecoder> BinkDecoder::open_memory(std::vector<uint8_t> bytes, const OpenOptions& opt,
                                                      std::string* err) {
    std::string e;
    const Api* ff = detail::api(&e);
    if (!ff) {
        if (err) *err = e;
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->ff = ff;
    impl->bytes = std::move(bytes);
    impl->opt = opt;
    if (!impl->open()) {
        if (err) *err = impl->err;
        return nullptr;
    }
    return std::unique_ptr<BinkDecoder>(new BinkDecoder(std::move(impl)));
}

const VideoInfo& BinkDecoder::info() const { return impl_->info; }
bool BinkDecoder::next_frame(VideoFrame& out) { return impl_->next_frame(out); }

size_t BinkDecoder::take_audio(std::vector<float>& interleaved) {
    auto& q = impl_->audio;
    if (q.empty() || impl_->info.channels <= 0) return 0;
    interleaved.insert(interleaved.end(), q.begin(), q.end());
    const size_t frames = q.size() / static_cast<size_t>(impl_->info.channels);
    q.clear();
    return frames;
}

int64_t BinkDecoder::audio_frames_decoded() const { return impl_->audio_total; }

bool BinkDecoder::rewind() {
    impl_->audio.clear();
    impl_->err.clear();
    const VideoInfo keep = impl_->info;
    if (!impl_->open()) {
        impl_->end = true;
        return false;
    }
    impl_->info = keep;
    return true;
}

bool BinkDecoder::at_end() const { return impl_->end; }
const std::string& BinkDecoder::error() const { return impl_->err; }

}  // namespace alamo::video
