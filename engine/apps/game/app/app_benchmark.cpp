// alamo_game App: BM2a benchmark mode (benchmark.hpp has the options, presets, fixed-frames clock, statistics and report).
//
//   alamo_game --benchmark realtime|fixed --bench-save FILE [--bench-preset NAME] [--bench-ticks N] [...]
//
// The run starts straight from the save (no intro, menus or attract demo), takes no player input (the idle frame: cursor
// parked, not inside the window), draws no cursor, tooltips, hover or frame-time overlay, plays no movies and reads no
// video options file: the preset sets the window size, present mode, frame cap and video options. What would wait for the
// player is answered at once, in the frame it appears (so the same tick in every run): the battle prompt = Begin (the
// battle is fought and drawn), the battle load screen = Begin, the battle-end screen = Continue, a story dialog = closed,
// a full-screen movie = finished (STORY_MOVIE_DONE sent), an advisor loop = stopped, a story summary = closed; a saved pause
// is lifted; a retry box (mission lost) fails the run. Sim-facing presentation timing is tied to ticks: the audio clock
// advances 1/30 s per tick (a speech's STORY_SPEECH_DONE), and every story UI notification raised between two ticks is sent
// just before the next one (bench_step_sim).
//   Phases: Loading (the save loads, its load screen) -> Settling (gameplay frames, the sim holds until settle_frames
//   settled frames in a row) -> Measuring (the measured ticks) -> Done. Real-time: the original pacing
//   (GameSession::loop_iteration, 30 ticks/s on the wall clock), every gameplay frame measured. Fixed frames: one tick per
//   K counted frames, the presentation clock on the counted frames, alpha (sub + 1) / K, blocking models; a frame counts
//   only when present_settled(), else it is drawn again. Frames outside gameplay (load screens, dialogs holding the sim)
//   are excluded in both modes. TAA (BM2b): the preset's in both modes. In fixed frames the TAA history restarts with the
//   measurement and every measured frame is drawn TAA-provisional: one that counts is kept, any other (unsettled,
//   excluded, a forced --bench-redraw-every redraw) is taken back (Renderer::discard_taa_frame), so the history and the
//   jitter cycle only ever see counted frames.
//   At the end: the report (stdout and --bench-report), the CRC log (--bench-crc-out) and, unless --bench-no-verify, the
//   presentation on/off check (crc_check: the save replayed with the run's recorded commands gives the same CRC log).
#include <filesystem>
#include <map>

#include "game_app.hpp"

namespace alamo_game_app {

namespace {
constexpr double kSettleLimitMs = 300000.0;  // the warm-up gives up after 5 minutes
constexpr double kStallLimitMs = 60000.0;    // no measured frame for a minute while measuring: stalled
}  // namespace

void App::bench_apply_window() {
    const BenchPreset& p = *find_bench_preset(opt_.bench.preset);  // checked in main (check_benchmark_options)
    if (!opt_.size_set) {
        opt_.width = p.width;
        opt_.height = p.height;
    }
    // Fixed frames: V-sync off and no cap, whatever the preset says (t03); real-time: the preset's.
    opt_.mode = bench_fixed() || !p.vsync ? rhi::PresentMode::Immediate : rhi::PresentMode::Fifo;
    opt_.frame_cap = bench_fixed() ? 0 : p.frame_cap;
    opt_.mode_set = opt_.frame_cap_set = true;
    opt_.audio = p.audio == BenchAudio::Off ? AudioBackendKind::Off : AudioBackendKind::Null;
}

void App::bench_apply_video() {
    const BenchPreset& p = *find_bench_preset(opt_.bench.preset);
    VideoProfile v;
    v.vsync = opt_.mode != rhi::PresentMode::Immediate;
    v.frame_cap = opt_.frame_cap;
    v.detail = p.detail;
    v.anti_alias = p.anti_alias;  // fixed frames too: redrawn frames are discarded from the TAA history (BM2b)
    v.gamma = p.gamma;
    ui::set_advanced_detail(v.advanced, p.detail);
    v.advanced.soft_shadows = p.soft_shadows;
    v.advanced.heat_distortion = p.heat_distortion;
    v.advanced.bloom = p.bloom;
    v.advanced.ambient_occlusion = p.ambient_occlusion;
    v.advanced.anisotropy = p.anisotropy;
    video_profile_ = v;
    adv_video_ = v.advanced;
    detail_options_ = true;
    hardware_mouse_ = false;
    std::printf("benchmark: preset %s (%s)\n", std::string(p.name).c_str(), std::string(p.description).c_str());
}

bool App::bench_start() {
    bench_.t_load = Clock::now();
    std::printf("benchmark: %s mode, %" PRIu64 " ticks%s, save %s, window %ux%u\n", bench_mode_name(opt_.bench.mode), opt_.bench.ticks,
                bench_fixed() ? (", " + std::to_string(opt_.bench.frames_per_tick) + " frames per tick").c_str() : "",
                opt_.bench.save.c_str(), win_.width(), win_.height());
    if (!opt_.bench.png_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt_.bench.png_dir, ec);  // a failure shows as FAILED PNG writes
    }
    if (!bench_scenario_start()) return true;  // bm2: the scenario's orders and the GPU timer (a failure is in the report)
    if (!load_save_file(opt_.bench.save, bench_replay())) {
        // A scenario's own save that no longer loads (older format) is a changed scenario, as in alamo_headless --bench.
        bscen_.not_comparable = opt_.bench.scenario_data && !opt_.bench.make;
        bench_fail(std::string(bscen_.not_comparable ? "scenario changed, not comparable: " : "") + "the save " + opt_.bench.save + " did not load");
        return true;  // init succeeded: run() writes the failed report
    }
    bench_.retry_boxes = retry_boxes_;
    if (session_->pause_requested()) {  // a game saved paused: the benchmark runs it
        toggle_pause();
        bench_.autopilot.push_back("saved pause lifted at tick " + std::to_string(session_->host().frame()));
    }
    return true;
}

double App::bench_now() const { return bench_fixed() ? bench_.clock.seconds() : ms_since(t0_) / 1000.0; }

float App::bench_alpha() const { return bench_measuring() ? bench_.clock.alpha() : 1.0f; }

void App::bench_fail(std::string why) {
    if (bench_done()) return;
    std::fprintf(stderr, "benchmark: %s\n", why.c_str());
    bench_.failed = true;
    bench_.error = std::move(why);
    if (bench_.t_start != Clock::time_point{}) bench_.wall = ms_since(bench_.t_start) / 1000.0;
    bench_.phase = BenchState::Phase::Done;
}

void App::bench_sample(const FrameTimes& closed) {
    if (bench_.counted) {
        bench_.frame_ms.push_back(closed.frame);
        bench_.render_ms.push_back(closed.render);
        if (bench_.ticked) bench_.sim_ms.push_back(closed.sim);
    }
    bench_.counted = bench_.ticked = bench_.tick_missed = false;  // the new frame's
}

void App::bench_step_sim() {
    if (!bench_measuring() || !session_) return;  // the warm-up holds the sim
    // The story UI notifications raised since the last tick, in order, right before the next one (no tick may run between
    // a notification and its submission, and none does: this is the only place that ticks).
    const auto flush_story = [this] {
        for (const sim::StoryUiNotification& n : bench_.story_pending)
            if (session_->notify_story_ui(n)) ++story_ui_sent_;
        bench_.story_pending.clear();
    };
    if (bench_fixed()) {
        if (!bench_.clock.tick_due() || bench_.clock.finished(opt_.bench.ticks)) return;
        flush_story();
        if (session_->tick()) {
            bench_.clock.ticked();
            bench_.ticked = true;
            post_tick();
        } else {
            bench_.tick_missed = true;  // this frame does not count: it would draw the old tick under the new sub-frame
        }
        return;
    }
    if (bench_.rt_ticks >= opt_.bench.ticks) return;
    const auto now = static_cast<uint32_t>(ms_since(t0_));
    flush_story();  // the host frame stays put until a tick, so "now" and "just before the next tick" are the same
    if (session_->loop_iteration(now)) {
        prev_tick_ms_ = last_tick_ms_;
        last_tick_ms_ = now;
        ++bench_.rt_ticks;
        bench_.ticked = true;
        post_tick();
    }
}

// BM3 --bench-trace N: every Nth measured tick one line per owner of the drawn battle's objects (count, centroid and the box they
// span, world units). Scene authoring only: where the fleets are, so a camera path can frame the fight.
void App::bench_trace() {
    if (opt_.bench.trace_every == 0 || !bench_measuring() || !bench_.ticked || !bview_.active()) return;
    const uint64_t ran = bench_fixed() ? bench_.clock.ticks() : bench_.rt_ticks;
    if (ran % opt_.bench.trace_every != 0) return;
    struct Acc {
        uint32_t n = 0;
        double c[3] = {0, 0, 0};
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    };
    std::map<int32_t, Acc> by_owner;
    for (const render::SnapshotObject& o : bview_.frame().objects) {
        if (o.owner < 0 || (o.flags & render::kSnapDrawable) == 0) continue;
        Acc& a = by_owner[o.owner];
        const float p[3] = {o.position.x, o.position.y, o.position.z};
        ++a.n;
        for (int k = 0; k < 3; ++k) {
            a.c[k] += static_cast<double>(p[k]);
            a.lo[k] = std::min(a.lo[k], p[k]);
            a.hi[k] = std::max(a.hi[k], p[k]);
        }
    }
    for (const auto& [owner, a] : by_owner)
        std::printf("trace: tick %" PRIu64 " (run %" PRIu64 ") owner %d: %u objects, centroid %.0f %.0f %.0f, box %.0f..%.0f x %.0f..%.0f x %.0f..%.0f\n",
                    session_->host().frame(), ran, owner, a.n, a.c[0] / a.n, a.c[1] / a.n, a.c[2] / a.n, static_cast<double>(a.lo[0]),
                    static_cast<double>(a.hi[0]), static_cast<double>(a.lo[1]), static_cast<double>(a.hi[1]), static_cast<double>(a.lo[2]),
                    static_cast<double>(a.hi[2]));
}

void App::bench_autopilot() {
    if (!session_ || bench_done()) return;
    const auto note = [this](const std::string& what) {
        const std::string line = what + " at tick " + std::to_string(session_ ? session_->host().frame() : 0);
        std::printf("benchmark: %s\n", line.c_str());
        bench_.autopilot.push_back(line);
    };
    if (screen_ == Screen::BattleLoad && ui_.load.is_open() && ui_.load.ready() && !battle_view_pending_ && load_intro_frames_ == 0) {
        ui_.load.close();  // as the Begin button (frame(): LoadScreenEvent::Begin)
        screen_ = Screen::Battle;
        audio_.music(MusicScene::Battle, local_faction(), bview_.space());
        note("load screen: Begin");
    }
    if (choice_open_) {
        answer_battle_prompt(false);
        note("battle prompt: Begin");
    }
    if (battle_end_open_ && ui_.gui.find(ui::dlg::kBattleEnd)) {
        ui_.gui.close(ui::dlg::kBattleEnd);
        ui::ScreenAction a;
        a.kind = ui::ScreenAction::Kind::BattleEndContinue;
        handle_screen_actions({a});
        note("battle-end screen: Continue");
    }
    if (story_summary_open_ && ui_.gui.find(ui::dlg::kStorySummary)) {  // a Story_Dialog_Popup chapter (holds the sim)
        ui_.gui.close(ui::dlg::kStorySummary);
        ui::ScreenAction a;
        a.kind = ui::ScreenAction::Kind::StorySummaryClosed;
        handle_screen_actions({a});
        note("story summary closed");
    }
    if (retry_boxes_ != bench_.retry_boxes) {  // Activate_Retry_Dialog: the mission was lost - no answer keeps the run valid
        bench_fail("the mission was lost (retry box) at tick " + std::to_string(session_->host().frame()));
        return;
    }
    if (ui_.gui.find(ui::dlg::kStory)) {
        ui_.gui.close(ui::dlg::kStory);
        note("story dialog closed");
    }
    if (full_movie_.playing()) {
        const std::string name = full_movie_.name();
        story_ui_.movie_done(name);  // STORY_MOVIE_DONE as at the movie's end, in the frame it started
        full_movie_.stop(audio_);
        note("movie " + name + " skipped");
    }
    if (advisor_movie_.playing()) advisor_movie_.stop(audio_);
}

void App::bench_frame_end() {
    using P = BenchState::Phase;
    BenchState& b = bench_;
    if (b.phase == P::Done) return;
    if (!session_ || screen_ == Screen::Menu || screen_ == Screen::Quit || campaign_over_) {
        bench_fail(!session_ ? "no session" : campaign_over_ ? "the campaign ended" : "the session ended (screen " + std::string(screen_name(screen_)) + ")");
        return;
    }
    const bool gameplay = (screen_ == Screen::Galactic || screen_ == Screen::Battle) && !ui_.load.is_open() && !battle_view_pending_ &&
                          load_intro_frames_ == 0 && !paused_ && !battle_end_open_ && !story_summary_open_ && !ui_.gui.has_focus();
    const bool settled = gameplay && b.drawn && present_settled() && !b.tick_missed;
    if (b.phase == P::Loading || b.phase == P::Settling) {
        ++b.warmup_frames;
        if (gameplay) b.phase = P::Settling;
        b.settled_run = settled ? b.settled_run + 1 : 0;
        if (b.phase == P::Settling && b.settled_run >= std::max(1u, opt_.bench.settle_frames)) {
            b.phase = P::Measuring;
            b.clock = FixedFrameClock(opt_.bench.frames_per_tick);
            b.start_tick = session_->host().frame();
            // BM2b: the warm-up's frame count varies from run to run; the measured frames start from an empty history and
            // the first jitter point.
            if (bench_fixed()) gpu_.renderer.reset_taa_history();
            b.t_start = b.t_progress = Clock::now();
            b.load_ms = ms_since(b.t_load);
            std::printf("benchmark: measuring from tick %" PRIu64 " (%s) after %" PRIu64 " warm-up frames, %.1f s\n", b.start_tick,
                        screen_name(screen_), b.warmup_frames, ms_since(b.t_load) / 1000.0);
        } else if (ms_since(b.t_load) > kSettleLimitMs) {
            bench_fail("the presentation did not settle within 300 s");
        }
        b.counted = false;  // warm-up frames are not measured
        return;
    }
    bool counted = false;
    if (bench_fixed()) {
        // --bench-redraw-every N: every Nth settled frame is drawn again as if it were unsettled (tests the redraw path).
        const bool forced = settled && opt_.bench.redraw_every && ++b.settled_seen % opt_.bench.redraw_every == 0;
        if (forced) ++b.forced;
        if (settled && !forced) {
            b.clock.frame_counted();
            counted = true;
            b.unsettled_run = 0;
        } else if (!gameplay) {
            ++b.excluded;
        } else {
            ++b.unsettled;
            if (++b.unsettled_run > opt_.bench.max_unsettled)
                bench_fail(std::to_string(b.unsettled_run) + " unsettled frames in a row at tick " + std::to_string(session_->host().frame()));
        }
        // BM2b: only a counted frame enters the TAA history and advances the jitter cycle.
        if (counted) gpu_.renderer.keep_taa_frame();
        else if (gpu_.renderer.discard_taa_frame()) ++b.taa_discarded;
    } else {
        counted = gameplay && b.drawn;
        if (!counted) ++b.excluded;
    }
    b.counted = counted;
    bench_trace();  // BM3
    bench_gpu_counted(counted);  // bm2: this frame's GPU timer scopes count only when the frame does
    if (counted) b.t_progress = Clock::now();
    else if (ms_since(b.t_progress) > kStallLimitMs) bench_fail("no measured frame for 60 s at tick " + std::to_string(session_->host().frame()));
    const bool finished = bench_fixed() ? b.clock.finished(opt_.bench.ticks) : b.rt_ticks >= opt_.bench.ticks;
    if (finished && !bench_done()) {
        b.wall = ms_since(b.t_start) / 1000.0;
        b.phase = P::Done;
    }
}

int App::bench_finish() {
    BenchState& b = bench_;
    if (!bench_done()) bench_fail("stopped before the end (window closed or --frames reached)");
    if (b.counted && frame_t0_ != Clock::time_point{}) {  // the last measured frame was not closed by a next frame
        FrameTimes last = ft_;
        last.frame = static_cast<float>(ms_since(frame_t0_));
        bench_sample(last);
    }
    BenchReport r;
    r.mode = bench_mode_name(opt_.bench.mode);
    r.preset = opt_.bench.preset;
    r.save = opt_.bench.save;
    r.device = ctx_.caps().name;
    r.width = targets_.width;
    r.height = targets_.height;
    const VkPresentModeKHR pm = sc_.present_mode();
    r.present_mode = pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    r.frame_cap = static_cast<int32_t>(limiter_.fps());
    r.taa = taa_active(video_profile_.anti_alias);
    r.frames_per_tick = bench_fixed() ? opt_.bench.frames_per_tick : 0;
    r.target_ticks = opt_.bench.ticks;
    r.start_tick = b.start_tick;
    r.ticks = bench_fixed() ? b.clock.ticks() : b.rt_ticks;
    r.frames = b.frame_ms.size();
    r.warmup_frames = b.warmup_frames;
    r.unsettled_frames = b.unsettled;
    r.excluded_frames = b.excluded;
    r.forced_redraws = b.forced;
    r.taa_discarded = b.taa_discarded;
    r.wall_seconds = b.wall;
    r.frame_ms = frame_time_stats(b.frame_ms);
    r.render_ms = frame_time_stats(b.render_ms);
    r.sim_ms = frame_time_stats(b.sim_ms);
    r.load_ms = b.load_ms;  // bm3
    bench_hitches(b.frame_ms, r);
    r.screen = screen_name(screen_);
    r.autopilot = b.autopilot;
    if (session_) {
        r.end_tick = session_->host().frame();
        r.final_crc = final_crc(session_->host().crc_log());
        if (!opt_.bench.crc_out.empty()) {
            std::string e;
            if (!session_->host().crc_log().save(opt_.bench.crc_out, &e)) std::fprintf(stderr, "benchmark: CRC log: %s\n", e.c_str());
        }
    }
    if (!b.failed && opt_.bench.verify && session_) {
        r.presentation_check = crc_check() ? "match" : "diverged";
        if (r.presentation_check != "match") {
            b.failed = true;
            b.error = "the presentation on/off check diverged";
        }
    }
    bench_scenario_end(r);  // bm2: GPU timer scopes, the scenario's fingerprint check, --bench-make's scenario
    r.result = b.failed ? "failed" : "ok";
    r.error = b.error;
    const std::string json = bench_report_json(r);
    std::printf("%s", json.c_str());
    std::printf("benchmark %s: %s, %" PRIu64 " ticks, %" PRIu64 " frames, frame ms median %.3f p99 %.3f worst %.3f, final CRC %016" PRIx64 "\n",
                r.mode.c_str(), r.result.c_str(), r.ticks, r.frames, r.frame_ms.median, r.frame_ms.p99, r.frame_ms.worst, r.final_crc);
    if (!opt_.bench.report.empty()) {
        if (FILE* f = std::fopen(opt_.bench.report.c_str(), "wb")) {
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
        } else {
            std::fprintf(stderr, "benchmark: cannot write %s\n", opt_.bench.report.c_str());
            return 1;
        }
    }
    return b.failed ? (bscen_.not_comparable ? 3 : 1) : 0;  // 3: scenario changed, not comparable (as alamo_headless --bench)
}

}  // namespace alamo_game_app
