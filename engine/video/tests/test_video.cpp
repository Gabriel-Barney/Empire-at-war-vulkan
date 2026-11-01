// alamo_video tests.
//   * offline: YUV->RGBA conversion, FFmpeg runtime load + licence, rejection of non-Bink data
//   * real data (ALAMO_FOC_DATA = FoC "corruption" folder; EaW GameData auto-detected by the VFS; skipped
//     when unset): every movie in Data\Art\Movies opens and decodes its first frames; one movie is decoded
//     in full (frame count, size, timing, audio length), RGBA vs YUV paths agree, MoviePlayer pacing on the
//     wall clock and on a simulated audio-device clock, looping; frame PNG written to the build dir.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/video/video.hpp"
#if defined(ALAMO_VIDEO_HAVE_VFS)
#include "alamo/vfs/vfs.hpp"
#endif
#if defined(ALAMO_VIDEO_HAVE_MOVIE_DB)
#include <string_view>

#include "alamo/data/xml.hpp"
#include "alamo/video/movie_db.hpp"
#endif

#ifndef ALAMO_VIDEO_OUT_DIR
#define ALAMO_VIDEO_OUT_DIR "."
#endif

namespace {

int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- minimal PNG writer (stored deflate) -----------------------------------------------------------------
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, alamo::crc32(td.data(), td.size()));
}

bool write_png(const std::string& path, const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        if (last) break;
    }
    put32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// ---- offline ---------------------------------------------------------------------------------------------
void test_yuv() {
    // 2x2 image, one chroma sample.
    auto conv = [](uint8_t yv, uint8_t uv, uint8_t vv, bool full, const uint8_t* alpha) {
        const uint8_t y[4] = {yv, yv, yv, yv};
        std::vector<uint8_t> out(16);
        alamo::video::yuv420_to_rgba(y, 2, &uv, 1, &vv, 1, alpha, alpha ? 2 : 0, 2, 2, full, out.data(), 8);
        return out;
    };
    auto white = conv(235, 128, 128, false, nullptr);
    check(white[0] == 255 && white[1] == 255 && white[2] == 255 && white[3] == 255, "yuv limited white");
    auto black = conv(16, 128, 128, false, nullptr);
    check(black[0] == 0 && black[1] == 0 && black[2] == 0, "yuv limited black");
    auto fwhite = conv(255, 128, 128, true, nullptr);
    check(fwhite[0] == 255 && fwhite[1] == 255 && fwhite[2] == 255, "yuv full white");
    auto gray = conv(128, 128, 128, true, nullptr);
    check(gray[0] == 128 && gray[1] == 128 && gray[2] == 128, "yuv full gray");
    // BT.601 limited red (R=255,G=0,B=0) is Y=81 U=90 V=240.
    auto red = conv(81, 90, 240, false, nullptr);
    check(red[0] >= 253 && red[1] <= 2 && red[2] <= 2, "yuv limited red");
    const uint8_t a[4] = {10, 20, 30, 40};
    auto al = conv(128, 128, 128, true, a);
    check(al[3] == 10 && al[7] == 20 && al[11] == 30 && al[15] == 40, "yuv alpha plane");
}

bool test_runtime() {
    std::string err;
    const bool ok = alamo::video::ffmpeg_available(&err);
    check(ok, "FFmpeg runtime loads");
    if (!ok) {
        std::printf("FFmpeg: %s\n", err.c_str());
        return false;
    }
    std::printf("%s\n", alamo::video::ffmpeg_version_string().c_str());
    const std::string lic = alamo::video::ffmpeg_license();
    check(lic.rfind("LGPL", 0) == 0, "FFmpeg build is LGPL");
    std::vector<uint8_t> junk(4096, 0x5A);
    std::string e2;
    auto d = alamo::video::BinkDecoder::open_memory(junk, {}, &e2);
    check(!d && !e2.empty(), "non-Bink data rejected");
    auto d2 = alamo::video::BinkDecoder::open_memory({}, {}, &e2);
    check(!d2, "empty data rejected");
    return true;
}

// ---- real data -------------------------------------------------------------------------------------------
#if defined(ALAMO_VIDEO_HAVE_VFS)
std::string fourcc(uint32_t t) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((t >> (8 * i)) & 0xFF);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

double rgba_spread(const alamo::video::VideoFrame& f) {
    // Standard deviation of luma-ish values: a decoded picture is not flat noise or a single colour.
    double sum = 0, sq = 0;
    const size_t n = f.data.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        const double v = (f.data[4 * i] + f.data[4 * i + 1] + f.data[4 * i + 2]) / 3.0;
        sum += v;
        sq += v * v;
    }
    const double m = sum / static_cast<double>(n);
    return std::sqrt(std::max(0.0, sq / static_cast<double>(n) - m * m));
}

void full_decode(const alamo::FileProvider& vfs, const std::string& path) {
    using namespace alamo::video;
    std::string err;
    auto dec = BinkDecoder::open(vfs, path, {}, &err);
    check(dec != nullptr, "full-decode movie opens");
    if (!dec) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    const VideoInfo vi = dec->info();
    VideoFrame f;
    int64_t n = 0;
    bool seq = true, size_ok = true, timing_ok = true;
    std::vector<float> pcm;
    while (dec->next_frame(f)) {
        if (f.index != n) seq = false;
        if (f.width != vi.width || f.height != vi.height || f.data.size() != static_cast<size_t>(vi.width) * static_cast<size_t>(vi.height) * 4u)
            size_ok = false;
        if (std::fabs(f.pts_s - static_cast<double>(n) * vi.fps_den / vi.fps_num) > 1e-9) timing_ok = false;
        ++n;
        dec->take_audio(pcm);
    }
    dec->take_audio(pcm);
    std::printf("full decode %s: %lld/%lld frames, %zu audio sample frames (%.2f s of %.2f s video) err='%s'\n",
                path.c_str(), static_cast<long long>(n), static_cast<long long>(vi.frame_count),
                vi.channels > 0 ? pcm.size() / static_cast<size_t>(vi.channels) : size_t{0},
                vi.sample_rate > 0 ? static_cast<double>(pcm.size()) / vi.channels / vi.sample_rate : 0.0,
                vi.duration_s, dec->error().c_str());
    check(n == vi.frame_count, "decoded frame count == header frame count");
    check(seq, "frame indices are 0..n-1");
    check(size_ok, "every frame has the header size");
    check(timing_ok, "frame pts = index / fps");
    if (vi.sample_rate > 0) {
        const double audio_s = static_cast<double>(pcm.size()) / vi.channels / vi.sample_rate;
        check(std::fabs(audio_s - vi.duration_s) < 0.1 * vi.duration_s + 0.5, "audio length ~ video length");
        float peak = 0;
        bool finite = true;
        for (float s : pcm) {
            if (!std::isfinite(s)) finite = false;
            peak = std::max(peak, std::fabs(s));
        }
        std::printf("audio peak %.4f\n", static_cast<double>(peak));
        check(finite && peak > 0.001f && peak < 4.0f, "audio PCM finite and audible");
    }

    // Player on the wall clock: 60 Hz host, audio pulled every update.
    {
        auto d2 = BinkDecoder::open(vfs, path, {}, &err);
        MoviePlayer player(std::move(d2));
        std::vector<float> buf(4096 * 8);
        int64_t shown = 0, updates = 0;
        uint64_t last_serial = 0;
        bool order_ok = true;
        int64_t last_index = -1;
        size_t pulled = 0;
        const double dt = 1.0 / 60.0;
        while (!player.finished() && updates < 200000) {
            player.update(dt);
            ++updates;
            if (player.frame_serial() != last_serial) {
                shown += static_cast<int64_t>(player.frame_serial() - last_serial);
                last_serial = player.frame_serial();
                if (player.current_frame()->index <= last_index) order_ok = false;
                last_index = player.current_frame()->index;
                // A shown frame is due: its time is <= the clock and the next frame's time is not past it by >1 frame.
                if (player.current_frame()->pts_s > player.time_s() + 1e-9) order_ok = false;
            }
            if (vi.sample_rate > 0) {
                const size_t want = static_cast<size_t>(vi.sample_rate / 60) + 1;
                pulled += player.read_audio(buf.data(), std::min(want, buf.size() / static_cast<size_t>(vi.channels)));
            }
        }
        std::printf("player (wall 60 Hz): %lld updates, %.3f s, %lld frames shown, %lld dropped, %zu audio pulled\n",
                    static_cast<long long>(updates), player.time_s(), static_cast<long long>(shown),
                    static_cast<long long>(player.frames_dropped()), pulled);
        check(player.finished(), "player finishes");
        check(order_ok, "player shows frames in order, never early");
        check(shown + player.frames_dropped() == vi.frame_count, "shown + dropped == frame count");
        check(std::fabs(player.time_s() - vi.duration_s) < 2.0 / 60.0 + 1e-6, "player ends at the movie duration");
    }
    // Player on the audio clock: a simulated device pulls 512-frame blocks at slightly fast speed (+2%).
    if (vi.sample_rate > 0) {
        auto d3 = BinkDecoder::open(vfs, path, {}, &err);
        MoviePlayer player(std::move(d3), MoviePlayer::Clock::Audio);
        std::vector<float> buf(512 * static_cast<size_t>(vi.channels));
        double device_t = 0, wall = 0;
        double max_lag = 0;
        int64_t updates = 0;
        while (!player.finished() && updates < 200000) {
            wall += 1.0 / 60.0;
            while (device_t < wall * 1.02) {
                const size_t got = player.read_audio(buf.data(), 512);
                device_t += 512.0 / vi.sample_rate;
                if (got == 0 && player.audio_queued_frames() == 0) break;
            }
            player.update(1.0 / 60.0);
            ++updates;
            const auto* cf = player.current_frame();
            if (cf) max_lag = std::max(max_lag, player.time_s() - cf->pts_s);
        }
        std::printf("player (audio clock, device +2%%): %lld updates, clock %.3f s, wall %.3f s, max frame lag %.3f s\n",
                    static_cast<long long>(updates), player.time_s(), wall, max_lag);
        check(player.finished(), "audio-clock player finishes");
        check(wall < vi.duration_s, "audio clock follows the faster device");
        check(max_lag < 3.0 * vi.fps_den / vi.fps_num + 1.0 / 30.0, "frames keep up with the audio clock");
    }
    // OI host (P3 video item 3): the output device latency is subtracted from the audio clock.
    if (vi.sample_rate > 0) {
        auto d4 = BinkDecoder::open(vfs, path, {}, &err);
        MoviePlayer player(std::move(d4), MoviePlayer::Clock::Audio);
        constexpr double kLatency = 0.1;
        player.set_audio_latency(kLatency);
        std::vector<float> buf(512 * static_cast<size_t>(vi.channels));
        double pulled_s = 0, wall = 0;
        bool behind = true, held = false, tracks = true;
        int64_t updates = 0;
        while (!player.finished() && updates < 200000) {
            wall += 1.0 / 60.0;
            while (pulled_s < wall) {
                const size_t got = player.read_audio(buf.data(), 512);
                pulled_s += static_cast<double>(got) / vi.sample_rate;
                if (got == 0) break;
            }
            player.update(1.0 / 60.0);
            ++updates;
            // The clock never runs backwards from 0: max(0, pulled - latency).
            if (player.audio_queued_frames() > 0 && player.time_s() > std::max(0.0, pulled_s - kLatency) + 1e-9) {
                if (behind)
                    std::printf("  latency clock ahead at update %lld: clock %.6f, pulled %.6f\n", static_cast<long long>(updates),
                                player.time_s(), pulled_s);
                behind = false;
            }
            if (pulled_s > 0.0 && pulled_s < kLatency && player.time_s() == 0.0) held = true;
            if (player.audio_queued_frames() > 0 && pulled_s > kLatency && std::fabs(player.time_s() - (pulled_s - kLatency)) > 1e-6) tracks = false;
        }
        std::printf("player (audio clock, latency %.2f s): %lld updates, clock %.3f s\n", kLatency, static_cast<long long>(updates),
                    player.time_s());
        check(player.finished(), "latency audio-clock player finishes");
        check(behind, "audio clock = handed-out samples / rate - latency");
        check(held, "first frame held until the first samples are audible");
        check(tracks, "audio clock tracks the device minus the latency");
    }
}

void test_real_data() {
    using namespace alamo::video;
    const std::string foc = alamo::testing::env_or_empty("ALAMO_FOC_DATA");
    if (foc.empty()) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = foc;
    const std::string eaw = alamo::testing::env_or_empty("ALAMO_EAW_DATA");
    if (!eaw.empty()) cfg.eaw_root = eaw;
    std::string err;
    auto vfs = alamo::vfs::Vfs::create(cfg, &err);
    check(vfs != nullptr, "VFS opens");
    if (!vfs) {
        std::printf("VFS: %s\n", err.c_str());
        return;
    }
    std::vector<std::string> movies;
    for (const auto& p : vfs->list("Data\\Art\\Movies\\**")) {
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".BIK") == 0) movies.push_back(p);
    }
    std::printf("movies in the VFS: %zu\n", movies.size());
    check(movies.size() >= 15, "FoC movies visible through the VFS (>= 15)");

    constexpr int kFirst = 10;
    int opened = 0;
    std::string biggest;
    double biggest_s = 0;
    for (const auto& m : movies) {
        OpenOptions so;
        so.format = PixelFormat::Yuv420p;  // the sweep skips the RGBA conversion
        auto dec = BinkDecoder::open(*vfs, m, so, &err);
        if (!dec) {
            std::printf("  %-55s OPEN FAILED: %s\n", m.c_str(), err.c_str());
            check(false, "movie opens");
            continue;
        }
        ++opened;
        const VideoInfo& vi = dec->info();
        VideoFrame f;
        int got = 0;
        while (got < kFirst && dec->next_frame(f)) ++got;
        std::string audio_note;
        if (vi.sample_rate > 0) {
            // Movies with sound are decoded to the end: all frames present, audio as long as the video.
            int64_t n = got;
            while (dec->next_frame(f)) ++n;
            std::vector<float> pcm;
            dec->take_audio(pcm);
            float peak = 0;
            for (float s : pcm) peak = std::max(peak, std::fabs(s));
            const double secs = static_cast<double>(pcm.size()) / vi.channels / vi.sample_rate;
            char buf[96];
            std::snprintf(buf, sizeof(buf), " | full: %lld frames, audio %.2f s peak %.3f", static_cast<long long>(n),
                          secs, static_cast<double>(peak));
            audio_note = buf;
            check(n == vi.frame_count, "audio movie: all frames decode");
            check(std::fabs(secs - vi.duration_s) < 0.1 * vi.duration_s + 0.5, "audio movie: audio length ~ video length");
        }
        std::printf("  %-55s %s %4dx%-4d %6.3f fps %5lld frames %7.2f s alpha=%d audio=%d (%d Hz x%d) first %d ok%s\n",
                    m.c_str(), fourcc(vi.codec_tag).c_str(), vi.width, vi.height,
                    static_cast<double>(vi.fps_num) / vi.fps_den, static_cast<long long>(vi.frame_count),
                    vi.duration_s, vi.has_alpha ? 1 : 0, vi.audio_tracks, vi.sample_rate, vi.channels, got, audio_note.c_str());
        check(vi.width > 0 && vi.height > 0 && vi.width <= 4096 && vi.height <= 4096, "movie size sane");
        check(vi.fps_num > 0 && vi.fps_den > 0, "movie fps sane");
        check(got == std::min<int64_t>(kFirst, vi.frame_count), "first frames decode");
        if (vi.sample_rate > 0 && (biggest.empty() || vi.duration_s < biggest_s)) {
            biggest_s = vi.duration_s;
            biggest = m;
        }
    }
    check(opened == static_cast<int>(movies.size()), "all movies open");

    // Sample frame + RGBA/YUV consistency + full decode on the shortest movie with audio (or the first movie).
    const std::string sample = !biggest.empty() ? biggest : (movies.empty() ? std::string() : movies.front());
    if (sample.empty()) return;
    {
        OpenOptions yo;
        yo.format = PixelFormat::Yuv420p;
        auto rd = BinkDecoder::open(*vfs, sample, {}, &err);
        auto yd = BinkDecoder::open(*vfs, sample, yo, &err);
        check(rd && yd, "sample movie opens twice");
        if (!rd || !yd) return;
        VideoFrame rf, yf;
        const int64_t target = std::min<int64_t>(rd->info().frame_count - 1, 45);
        bool same = true;
        for (int64_t i = 0; i <= target; ++i) {
            if (!rd->next_frame(rf) || !yd->next_frame(yf)) {
                same = false;
                break;
            }
            std::vector<uint8_t> conv(static_cast<size_t>(yf.width) * static_cast<size_t>(yf.height) * 4u);
            const uint8_t* base = yf.data.data();
            yuv420_to_rgba(base + yf.plane_offset[0], yf.plane_stride[0], base + yf.plane_offset[1], yf.plane_stride[1],
                           base + yf.plane_offset[2], yf.plane_stride[2],
                           yf.plane_count == 4 ? base + yf.plane_offset[3] : nullptr, yf.plane_stride[3], yf.width,
                           yf.height, yf.full_range, conv.data(), yf.width * 4);
            if (conv != rf.data) same = false;
        }
        check(same, "RGBA output == YUV planes converted (first frames)");
        check(yf.plane_count >= 3 && yf.plane_stride[1] == (yf.width + 1) / 2, "YUV plane layout");
        const double spread = rgba_spread(rf);
        std::printf("sample %s frame %lld: luma stddev %.1f\n", sample.c_str(), static_cast<long long>(rf.index), spread);
        check(spread > 2.0, "sample frame has picture content");
        const std::string png = std::string(ALAMO_VIDEO_OUT_DIR) + "/video_frame.png";
        check(write_png(png, rf.data.data(), rf.width, rf.height), "sample PNG written");
        std::printf("wrote %s (%dx%d)\n", png.c_str(), rf.width, rf.height);
        // Rewind gives frame 0 again, identical.
        VideoFrame a0, b0;
        auto d4 = BinkDecoder::open(*vfs, sample, {}, &err);
        d4->next_frame(a0);
        d4->next_frame(b0);
        check(d4->rewind(), "rewind");
        d4->next_frame(b0);
        check(b0.index == 0 && b0.data == a0.data, "rewind -> identical frame 0");
    }
    full_decode(*vfs, sample);

    // Looping: two passes worth of frames with monotonically increasing player time.
    {
        auto d5 = BinkDecoder::open(*vfs, sample, {}, &err);
        const VideoInfo vi = d5->info();
        MoviePlayer player(std::move(d5));
        player.set_loop(true);
        const double fdt = static_cast<double>(vi.fps_den) / vi.fps_num;
        uint64_t serial = 0;
        int wraps = 0;
        int64_t last = -1;
        for (int64_t i = 0; i < vi.frame_count + 20; ++i) {
            player.update(fdt);
            if (player.frame_serial() != serial) {
                serial = player.frame_serial();
                const int64_t idx = player.current_frame()->index;
                if (idx < last) ++wraps;
                last = idx;
            }
        }
        check(!player.finished() && wraps == 1, "looping player wraps once and keeps going");
    }
#if defined(ALAMO_VIDEO_HAVE_MOVIE_DB)
    // Alpha movies (STRATEGIC_GAPS §7.3): Movies.xml Alpha -> the alpha plane is decoded (straight alpha in RGBA A),
    // OpenOptions::alpha = false ignores it (A = 255, same colour); movies without Alpha have no plane.
    {
        std::vector<uint8_t> xml;
        check(vfs->read("Data\\XML\\Movies.xml", xml), "Movies.xml reads");
        const alamo::data::XmlDocument doc =
            alamo::data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()), "Movies.xml");
        const std::vector<MovieEntry> db = parse_movies(doc.root());
        size_t alpha_entries = 0, alpha_ok = 0, alpha_open = 0;
        const MovieEntry* sample_alpha = nullptr;
        for (const MovieEntry& m : db) {
            if (!m.alpha || m.file.empty()) continue;
            ++alpha_entries;
            auto d = BinkDecoder::open(*vfs, m.file, open_options_for(m, PixelFormat::Yuv420p, -1), &err);
            if (!d) continue;  // a listed file missing from the install
            ++alpha_open;
            if (d->info().file_alpha && d->info().has_alpha) ++alpha_ok;
            if (!sample_alpha && d->info().file_alpha) sample_alpha = &m;
        }
        std::printf("Movies.xml: %zu entries, %zu with Alpha, %zu of them open, %zu decode an alpha plane\n", db.size(), alpha_entries,
                    alpha_open, alpha_ok);
        check(db.size() > 20 && alpha_entries > 10, "Movies.xml parsed (Alpha entries present)");
        check(alpha_open > 0 && alpha_ok == alpha_open, "every Alpha movie decodes an alpha plane");
        const MovieEntry* logo = find_movie(db, "Logo1");
        check(logo && !logo->alpha && logo->file.find("LucasArtsLogo") != std::string::npos, "Logo1 entry (no Alpha)");
        check(find_movie(db, "pirate_loop2") != nullptr && find_movie(db, "pirate_loop2")->alpha, "find_movie by file name");
        if (logo) {
            auto ld = BinkDecoder::open(*vfs, logo->file, open_options_for(*logo, PixelFormat::Rgba8, -1), &err);
            check(ld && !ld->info().has_alpha, "opaque movie: no alpha plane in the output");
        }
        if (sample_alpha) {
            OpenOptions on = open_options_for(*sample_alpha, PixelFormat::Rgba8, -1), off = on;
            off.alpha = false;
            auto da = BinkDecoder::open(*vfs, sample_alpha->file, on, &err);
            auto db2 = BinkDecoder::open(*vfs, sample_alpha->file, off, &err);
            check(da && db2 && da->info().has_alpha && !db2->info().has_alpha && db2->info().file_alpha, "alpha on / off info");
            VideoFrame fa, fb;
            bool same_rgb = true, any_transparent = false, b_opaque = true;
            for (int i = 0; da && db2 && i < 10 && da->next_frame(fa) && db2->next_frame(fb); ++i) {
                for (size_t p = 0; p + 3 < fa.data.size() && p + 3 < fb.data.size(); p += 4) {
                    same_rgb = same_rgb && fa.data[p] == fb.data[p] && fa.data[p + 1] == fb.data[p + 1] && fa.data[p + 2] == fb.data[p + 2];
                    any_transparent = any_transparent || fa.data[p + 3] < 255;
                    b_opaque = b_opaque && fb.data[p + 3] == 255;
                }
            }
            check(same_rgb && any_transparent && b_opaque, "alpha movie: straight alpha in A, ignored plane = opaque");
            std::printf("alpha sample %s (%s): transparent texels %d\n", sample_alpha->name.c_str(), sample_alpha->file.c_str(),
                        any_transparent ? 1 : 0);
        }
    }
#endif
}
#endif

}  // namespace

int main() {
    test_yuv();
    if (test_runtime()) {
#if defined(ALAMO_VIDEO_HAVE_VFS)
        test_real_data();
#else
        std::printf("real data: vfs module not configured, skipped\n");
#endif
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
