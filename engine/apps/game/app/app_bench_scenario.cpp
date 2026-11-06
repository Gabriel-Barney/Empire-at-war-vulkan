// alamo_game App: bm2 rendered benchmark runs (benchmark_scenario.hpp has the file side).
//
//   --bench-scenario S.abench   the scenario's save loads with its recorded orders as the session's replay (the run's own
//                               commands, e.g. the autopilot's battle choice, are ignored by the pipeline: the stream has
//                               them at the same ticks); at the end the run's CRC log is checked against the fingerprint
//                               ("scenario changed, not comparable", exit 3, when it differs).
//   --bench-make S.abench       a normal benchmark run from --bench-save; when it passed (presentation check included) the
//                               save, the run's recorded commands and its CRC log are written as the scenario S.
//   Camera (format-2 `camera` keys): from the first gameplay frame of the run on (warm-up included, so the settle covers
//   the path's first view) the drawn world view's camera is replaced by camera_pose(keys, camera_time(ticks run, alpha)):
//   a function of the measured tick count and the frame's render alpha only, never of wall-clock time. The path drives
//   the view (galactic or battle) shown at that first frame; the other view keeps its own camera. camera_anchor start
//   adds that view's own camera target at that frame to every key's target.
//   GPU timers (t06): a GpuTimer over every frame ("frame" around the whole command buffer, the renderer's own scopes
//   inside: world / scene / ao / post / taa); a slot's results are read when the slot comes round again (FrameRing
//   guarantees that frame finished) and kept only when that frame counted; the last frames are read after a wait_idle.
#include "game_app.hpp"

namespace alamo_game_app {

bool App::bench_scenario_start() {
    const sim::bench::Scenario* sc = opt_.bench.scenario_data.get();
    if (sc && !opt_.bench.make && sc->orders.present()) {
        std::string err;
        if (!commands::CommandRecording::load(sc->resolve(sc->orders), bscen_.orders, &err)) {
            bench_fail("the scenario's orders: " + err);
            return false;
        }
        bscen_.have_orders = true;
    }
    if (sc)
        std::printf("benchmark: scenario %s version %u (%s, %zu camera keys%s)%s\n", sc->name.c_str(), sc->version,
                    opt_.bench.make ? "make" : "run", sc->camera.size(), sc->camera_anchor == sim::bench::CameraAnchor::Start ? ", anchor start" : "",
                    bscen_.have_orders ? ", replaying its orders" : "");
    bscen_.gpu_on = bscen_.gpu.init(ctx_, gpu_.ring.frames_in_flight(), 64);
    bscen_.slot_counted.assign(gpu_.ring.frames_in_flight(), 0);
    if (!bscen_.gpu_on) std::printf("benchmark: no GPU timestamps on this queue (no GPU timer scopes)\n");
    return true;
}

bool App::bench_camera(ui::CameraParams& cp, int view, uint32_t w, uint32_t h) {
    const sim::bench::Scenario* sc = opt_.bench.scenario_data.get();
    if (!bench_active() || !sc || sc->camera.empty() || !session_ || bench_.phase == BenchState::Phase::Loading) return false;
    if (bscen_.view == 0) {  // the first gameplay frame: this view is the path's
        bscen_.view = view;
        bscen_.anchor = cp.target;
    }
    if (view != bscen_.view) return false;
    const uint64_t ran = bench_measuring() || bench_done() ? session_->host().frame() - bench_.start_tick : 0;
    const float alpha = bench_fixed() ? bench_alpha() : render_alpha();
    const sim::bench::CameraKey p = sim::bench::camera_pose(sc->camera, sim::bench::camera_time(ran, alpha));
    ui::Vec3 t{p.target[0], p.target[1], p.target[2]};
    if (sc->camera_anchor == sim::bench::CameraAnchor::Start) t = t + bscen_.anchor;
    const float yaw = p.yaw_deg * ui::kDegToRad, pitch = p.pitch_deg * ui::kDegToRad;
    const ui::Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    cp.target = t;
    cp.eye = t + dir * p.distance;
    cp.up = {0, 0, 1};
    cp.roll = 0.0f;
    cp.orbit_yaw = yaw;
    cp.orbit_pitch = pitch;
    cp.distance = p.distance;
    if (p.fov_deg > 0.0f)
        cp.fov_x = view == 2 && h ? ui::tactical_fov_x(p.fov_deg, static_cast<float>(w) / static_cast<float>(h)) : p.fov_deg * ui::kDegToRad;
    return true;
}

void App::bench_gpu_begin(rhi::Frame& f, VkCommandBuffer cmd) {
    bscen_.gpu_slot = UINT32_MAX;
    bscen_.gpu_frame_scope = UINT32_MAX;
    if (!bscen_.gpu_on) return;
    const uint32_t slot = f.slot();
    bscen_.gpu.begin_frame(cmd, slot);  // reads the slot's last use (finished: FrameRing handed the slot out again)
    if (slot < bscen_.slot_counted.size() && bscen_.slot_counted[slot]) {
        ++bscen_.gpu_frames;
        for (const rhi::GpuTimerResult& res : bscen_.gpu.results()) {
            auto it = std::find_if(bscen_.gpu_ms.begin(), bscen_.gpu_ms.end(), [&](const auto& e) { return e.first == res.name; });
            if (it == bscen_.gpu_ms.end()) it = bscen_.gpu_ms.insert(bscen_.gpu_ms.end(), {res.name, {}});
            it->second.push_back(static_cast<float>(res.ms));
        }
    }
    if (slot < bscen_.slot_counted.size()) bscen_.slot_counted[slot] = 0;
    bscen_.gpu_slot = slot;
    bscen_.gpu_frame_scope = bscen_.gpu.begin_scope(cmd, "frame");
}

void App::bench_gpu_end(VkCommandBuffer cmd) {
    if (bscen_.gpu_on && bscen_.gpu_frame_scope != UINT32_MAX) bscen_.gpu.end_scope(cmd, bscen_.gpu_frame_scope);
    bscen_.gpu_frame_scope = UINT32_MAX;
}

void App::bench_gpu_counted(bool counted) {
    if (bscen_.gpu_slot < bscen_.slot_counted.size()) bscen_.slot_counted[bscen_.gpu_slot] = counted ? 1 : 0;
    bscen_.gpu_slot = UINT32_MAX;
}

void App::bench_scenario_end(BenchReport& r) {
    BenchState& b = bench_;
    // GPU timers: the frames still in flight, then the stats per scope.
    if (bscen_.gpu_on) {
        gpu_.ring.wait_idle();
        for (uint32_t slot = 0; slot < bscen_.slot_counted.size(); ++slot) {
            if (!bscen_.slot_counted[slot] || !bscen_.gpu.collect(slot)) continue;
            ++bscen_.gpu_frames;
            for (const rhi::GpuTimerResult& res : bscen_.gpu.results()) {
                auto it = std::find_if(bscen_.gpu_ms.begin(), bscen_.gpu_ms.end(), [&](const auto& e) { return e.first == res.name; });
                if (it == bscen_.gpu_ms.end()) it = bscen_.gpu_ms.insert(bscen_.gpu_ms.end(), {res.name, {}});
                it->second.push_back(static_cast<float>(res.ms));
            }
        }
        bscen_.gpu.shutdown();
        bscen_.gpu_on = false;
        r.gpu_timestamps = true;
    }
    r.gpu_frames = bscen_.gpu_frames;
    for (const auto& [name, ms] : bscen_.gpu_ms) r.gpu_ms.emplace_back(name, frame_time_stats(ms));
    const sim::bench::Scenario* sc = opt_.bench.scenario_data.get();
    if (!sc) return;
    r.scenario = opt_.bench.scenario;
    r.scenario_version = sc->version;
    r.camera_keys = sc->camera.size();
    if (!session_ || b.failed) return;
    if (!opt_.bench.make) {  // the fingerprint: the same work as the run that made the scenario
        if (b.start_tick != sc->start_tick) {
            r.fingerprint = "the run started at tick " + std::to_string(b.start_tick) + ", the scenario at " + std::to_string(sc->start_tick);
        } else {
            const std::string why = check_bench_fingerprint(*sc, session_->host().crc_log(), r.end_tick);
            r.fingerprint = why.empty() ? "match" : why;
        }
        if (r.fingerprint != "match") {
            bscen_.not_comparable = true;
            b.failed = true;
            b.error = "scenario changed, not comparable: " + r.fingerprint;
            std::printf("benchmark: %s\n", b.error.c_str());
        }
        return;
    }
    // --bench-make: only a run whose recorded orders replay to the same CRC log (the presentation check) becomes a scenario.
    if (r.presentation_check != "match") {
        b.failed = true;
        b.error = "--bench-make: the presentation check did not match, no scenario written";
        return;
    }
    const uint64_t save_tick = session_->load_point().tick;
    if (b.start_tick != save_tick) {
        b.failed = true;
        b.error = "--bench-make: the measurement started at tick " + std::to_string(b.start_tick) + ", not at the save's tick " +
                  std::to_string(save_tick);
        return;
    }
    std::string summary, err;
    const sim::bench::Scenario* base = sc->name.empty() ? nullptr : sc;
    if (!write_game_scenario(opt_.bench.scenario, base, opt_.bench.save, recording_, session_->host().crc_log(), save_tick, r.ticks,
                             opt_.bench, &summary, &err)) {
        b.failed = true;
        b.error = "--bench-make: " + err;
        return;
    }
    r.scenario_made = opt_.bench.scenario;
    std::printf("bench-make: %s\n", summary.c_str());
}

}  // namespace alamo_game_app
