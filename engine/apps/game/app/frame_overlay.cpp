// alamo_game OPT1 t07: the frame-time overlay (frame_overlay.hpp).
#include "frame_overlay.hpp"

#include <algorithm>
#include <cstdio>

#include "alamo/ui/hud_font.hpp"

namespace alamo_game_app {

using namespace alamo;

namespace {

constexpr float kScaleMs = 40.0f;  // graph top
constexpr float kBudgetP99Ms = 1000.0f / 60.0f, kBudgetTickMs = 1000.0f / 30.0f;

float ms(uint32_t ns) { return static_cast<float>(ns) / 1e6f; }

// Nearest-rank percentile (the profiler report's rule).
float pct(std::vector<float> v, double p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
    return v[i];
}

std::u32string ascii32(const char* s) {
    std::u32string o;
    for (; *s; ++s) o.push_back(static_cast<char32_t>(static_cast<unsigned char>(*s)));
    return o;
}

// Pixel rectangle (top-left origin) as a HUD quad on the overlay's layer.
void rect(ui::HudDrawList& dl, const ui::HudCanvas& c, float x0, float y0, float x1, float y1, ui::Rgba8 color) {
    ui::HudQuad q;
    q.min = c.pixel_to_canvas(x0, y1);
    q.max = c.pixel_to_canvas(x1, y0);
    q.color = color;
    dl.add(ui::kHudLayers - 1, ui::HudList::Alpha, q);
}

}  // namespace

void FrameOverlay::set_shown(bool on, bool keep_profiler) {
    if (on && !profile::enabled()) {
        profile::start(1024);
        started_profiler_ = true;
    }
    if (!on && started_profiler_ && !keep_profiler) {
        profile::stop();
        started_profiler_ = false;
    }
    if (on && !shown_) {  // a fresh window (the ring may have restarted)
        ticks_.clear();
        seen_ = profile::ticks_recorded();
    }
    shown_ = on;
}

void FrameOverlay::add_frame(const FrameTimes& t) {
    frames_.push_back(t);
    if (frames_.size() > kFrames) frames_.pop_front();
    if (log_) log_frames_.push_back(t);
}

void FrameOverlay::read_ticks() {
    const uint64_t n = profile::ticks_recorded();
    if (n < seen_) {  // the profiler restarted
        ticks_.clear();
        seen_ = 0;
    }
    if (n == seen_) return;
    const uint64_t fresh = std::min<uint64_t>(n - seen_, kTicks);
    seen_ = n;
    const uint32_t zc = std::min(profile::zone_count(), profile::kMaxZones);
    for (const profile::TickRecord& r : profile::snapshot(static_cast<uint32_t>(fresh))) {
        Tick t;
        t.frame = r.frame;
        t.total_ns = r.total_ns;
        for (uint32_t z = 0; z < zc; ++z)
            if (r.zone_ns[z] > t.worst_ns) {
                t.worst_ns = r.zone_ns[z];
                t.worst = static_cast<profile::ZoneId>(z);
            }
        ticks_.push_back(t);
        if (ticks_.size() > kTicks) ticks_.pop_front();
    }
}

void FrameOverlay::build(ui::HudDrawList& dl, const ui::HudCanvas& c, ui::FontCache& fonts) const {
    ui::HudTextStyle st;
    st.font = fonts.font("Arial", 7.5f, c.font_height());
    if (st.font < 0) return;
    st.color = {235, 235, 235, 255};
    st.outline = true;
    const float lh = static_cast<float>(std::max(fonts.metrics(st.font).line_height, 10));
    const float pad = 6.0f, w = 384.0f, tick_h = 60.0f, frame_h = 40.0f;
    const float x0 = static_cast<float>(c.screen_width) - w - 8.0f, y0 = 8.0f;
    const float h = pad + 4.0f * lh + tick_h + 4.0f + frame_h + pad;
    rect(dl, c, x0, y0, x0 + w, y0 + h, {0, 0, 0, 170});
    auto line = [&](int row, const char* text) {
        const float y = y0 + pad + (static_cast<float>(row) + 0.5f) * lh;
        ui::add_text(dl, ui::kHudLayers - 1, ui::HudList::Text, fonts, c, ascii32(text), c.pixel_to_canvas(x0 + pad, y), st);
    };
    char buf[192];
    // Ticks: last / p50 / p99 / max over the window, the worst tick and its biggest zone.
    std::vector<float> tv;
    const Tick* worst = nullptr;
    for (const Tick& t : ticks_) {
        tv.push_back(ms(t.total_ns));
        if (!worst || t.total_ns > worst->total_ns) worst = &t;
    }
    if (!profile::enabled()) std::snprintf(buf, sizeof buf, "sim tick: profiler off");
    else if (tv.empty()) std::snprintf(buf, sizeof buf, "sim tick: no ticks yet");
    else
        std::snprintf(buf, sizeof buf, "sim tick ms  last %.2f  p50 %.2f  p99 %.2f  max %.2f  (%zu)", static_cast<double>(tv.back()),
                      static_cast<double>(pct(tv, 0.50)), static_cast<double>(pct(tv, 0.99)), static_cast<double>(pct(tv, 1.0)), tv.size());
    line(0, buf);
    if (worst && worst->worst != profile::kNoZone)
        std::snprintf(buf, sizeof buf, "worst tick %llu: %.2f ms, zone %s %.2f ms", static_cast<unsigned long long>(worst->frame),
                      static_cast<double>(ms(worst->total_ns)), profile::zone_name(worst->worst), static_cast<double>(ms(worst->worst_ns)));
    else
        std::snprintf(buf, sizeof buf, "worst tick: -");
    line(1, buf);
    // Frames: p50 / p99 and the mean of each phase.
    std::vector<float> fv;
    FrameTimes mean;
    for (const FrameTimes& f : frames_) {
        fv.push_back(f.frame);
        mean.sim += f.sim;
        mean.prepare += f.prepare;
        mean.render += f.render;
        mean.wait += f.wait;
    }
    const float nf = frames_.empty() ? 1.0f : static_cast<float>(frames_.size());
    std::snprintf(buf, sizeof buf, "frame ms  p50 %.2f  p99 %.2f  max %.2f", static_cast<double>(pct(fv, 0.50)), static_cast<double>(pct(fv, 0.99)),
                  static_cast<double>(pct(fv, 1.0)));
    line(2, buf);
    std::snprintf(buf, sizeof buf, "mean  sim %.2f  prepare %.2f  render %.2f  wait %.2f", static_cast<double>(mean.sim / nf),
                  static_cast<double>(mean.prepare / nf), static_cast<double>(mean.render / nf), static_cast<double>(mean.wait / nf));
    line(3, buf);
    // Graphs: one bar per tick (green under 16.7 ms, yellow under 33.3, red above), one stacked bar per frame (sim orange,
    // render blue, its wait grey, the rest dark grey). 0..40 ms; the budget lines on top.
    const float gx = x0 + pad, gw = w - 2.0f * pad;
    const float ty = y0 + pad + 4.0f * lh, fy = ty + tick_h + 4.0f;
    rect(dl, c, gx, ty, gx + gw, ty + tick_h, {255, 255, 255, 18});
    rect(dl, c, gx, fy, gx + gw, fy + frame_h, {255, 255, 255, 18});
    const float bw = gw / static_cast<float>(kTicks);
    auto bar_px = [](float v, float height) { return std::min(v, kScaleMs) / kScaleMs * height; };
    for (size_t i = 0; i < ticks_.size(); ++i) {
        const float v = ms(ticks_[i].total_ns);
        const float x = gx + bw * static_cast<float>(kTicks - ticks_.size() + i);
        const ui::Rgba8 col = v < kBudgetP99Ms ? ui::Rgba8{80, 220, 100, 230} : v < kBudgetTickMs ? ui::Rgba8{240, 200, 60, 230} : ui::Rgba8{240, 70, 60, 240};
        rect(dl, c, x, ty + tick_h - std::max(bar_px(v, tick_h), 1.0f), x + std::max(bw - 1.0f, 1.0f), ty + tick_h, col);
    }
    for (size_t i = 0; i < frames_.size(); ++i) {
        const FrameTimes& f = frames_[i];
        const float x = gx + bw * static_cast<float>(kFrames - frames_.size() + i), x1 = x + std::max(bw - 1.0f, 1.0f);
        float base = 0.0f;
        auto seg = [&](float v, ui::Rgba8 col) {
            const float a = bar_px(base, frame_h), b = bar_px(base + std::max(v, 0.0f), frame_h);
            if (b > a) rect(dl, c, x, fy + frame_h - b, x1, fy + frame_h - a, col);
            base += std::max(v, 0.0f);
        };
        seg(f.sim, {240, 150, 50, 230});
        seg(f.render - f.wait, {80, 140, 240, 230});
        seg(f.wait, {150, 150, 150, 200});
        seg(f.frame - f.sim - f.render, {90, 90, 90, 200});
    }
    auto budget_lines = [&](float y, float gh) {
        const float a = y + gh - bar_px(kBudgetP99Ms, gh), b = y + gh - bar_px(kBudgetTickMs, gh);
        rect(dl, c, gx, a, gx + gw, a + 1.0f, {255, 255, 255, 110});
        rect(dl, c, gx, b, gx + gw, b + 1.0f, {255, 80, 80, 160});
    };
    budget_lines(ty, tick_h);
    budget_lines(fy, frame_h);
}

std::string FrameOverlay::report() const {
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof buf, "=== alamo_game frames: %zu\n", log_frames_.size());
    out += buf;
    if (log_frames_.empty()) return out;
    out += "phase (ms)          mean      p50      p99      max\n";
    auto row = [&](const char* name, float FrameTimes::*m) {
        std::vector<float> v;
        double sum = 0;
        for (const FrameTimes& f : log_frames_) {
            v.push_back(f.*m);
            sum += static_cast<double>(f.*m);
        }
        std::snprintf(buf, sizeof buf, "%-14s %9.3f %8.3f %8.3f %8.3f\n", name, sum / static_cast<double>(v.size()),
                      static_cast<double>(pct(v, 0.50)), static_cast<double>(pct(v, 0.99)), static_cast<double>(pct(v, 1.0)));
        out += buf;
    };
    row("frame", &FrameTimes::frame);
    row("sim", &FrameTimes::sim);
    row("prepare", &FrameTimes::prepare);
    row("render", &FrameTimes::render);
    row("wait", &FrameTimes::wait);
    return out;
}

}  // namespace alamo_game_app
