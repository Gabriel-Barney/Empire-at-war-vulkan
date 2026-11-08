// Q1: per-tick CRC harness on the job system. A toy sim that uses every parallel pattern the
// engine allows (per-index parallel_for, ordered parallel_reduce, map + ordered commit with RNG
// draws, Deferred background results, dmath) must give bit-identical CRC sequences for 0/1/4/N
// workers, and the compare must pinpoint an injected divergence.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/jobs/jobs.hpp"
#include "alamo/test/support.hpp"

namespace dm = alamo::dmath;
using namespace alamo::jobs;

using ::alamo::test::g_fail;

namespace {

struct ToySim {
    static constexpr uint32_t kN = 6000;
    std::vector<float> x, y, vx, vy;
    float energy = 0.0f;
    std::vector<uint32_t> events;
    alamo::GameRandom rng{0x51u, 0x99u};
    std::unique_ptr<Deferred<std::vector<float>>> histogram;
    float hist_sum = 0.0f;

    alamo::CrcRegistry reg;

    explicit ToySim(JobSystem& js) {
        alamo::SimRng init(2026);
        for (uint32_t i = 0; i < kN; ++i) {
            x.push_back(init.range_float(-500.0f, 500.0f));
            y.push_back(init.range_float(-500.0f, 500.0f));
            vx.push_back(init.range_float(-2.0f, 2.0f));
            vy.push_back(init.range_float(-2.0f, 2.0f));
        }
        histogram = std::make_unique<Deferred<std::vector<float>>>(js);

        reg.add("particles", [this](alamo::StateHasher& h) {
            h.u32(static_cast<uint32_t>(x.size()));
            h.span(x);
            h.span(y);
            h.span(vx);
            h.span(vy);
        });
        reg.add("energy", [this](alamo::StateHasher& h) {
            h.f32(energy);
            h.f32(hist_sum);
            h.u64(histogram->version());
            h.u32(static_cast<uint32_t>(histogram->current().size()));
            h.span(histogram->current());
        });
        reg.add("events", [this](alamo::StateHasher& h) {
            h.u32(static_cast<uint32_t>(events.size()));
            h.span(events);
        });
        reg.add("rng", [this](alamo::StateHasher& h) { rng.hash(h); });
        reg.add_parity("rng", [this] { return alamo::parity_rng_crc(rng.synced().state()); });
    }

    void add_systems(TickScheduler& sched, JobSystem& js) {
        sched.track(*histogram);
        sched.add_system("move", 1, 0, [this, &js](const TickContext&) {
            js.parallel_for(kN, 256, [this](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) {
                    const float ang = dm::atan2(vy[i], vx[i]) + 0.01f;
                    const float spd = dm::hypot(vx[i], vy[i]);
                    float s, c;
                    dm::sincos(ang, s, c);
                    vx[i] = c * spd;
                    vy[i] = s * spd;
                    x[i] += vx[i];
                    y[i] += vy[i];
                }
            });
        });
        sched.add_system("energy", 1, 0, [this, &js](const TickContext& ctx) {
            energy = parallel_reduce(
                js, kN, 256, 0.0f,
                [this](uint32_t b, uint32_t e) {
                    float s = 0.0f;
                    for (uint32_t i = b; i < e; ++i) s += dm::exp(-dm::sqrt(x[i] * x[i] + y[i] * y[i]) * 0.001f);
                    return s;
                },
                [](float a, float b) { return a + b; });
            if (ctx.frame % 30 == 0 && !(histogram->in_flight() && ctx.frame < histogram->ready_frame())) {
                std::vector<float> snap = x;  // inputs snapshotted at kick
                histogram->kick(ctx.frame, 7, [snap = std::move(snap)](std::vector<float>& out) {
                    out.assign(16, 0.0f);
                    for (float v : snap) {
                        const auto bin = static_cast<uint32_t>(dm::log2(1.0f + dm::sqrt(v * v)) * 1.5f) & 15u;
                        out[bin] += 1.0f;
                    }
                });
            }
            float hs = 0.0f;
            for (float v : histogram->current()) hs += v;
            hist_sum = hs;
        });
        sched.add_system("spawn", 5, 2, [this, &js](const TickContext& ctx) {
            parallel_map_then_ordered_commit(
                js, 64, 4,
                [this, &ctx](uint32_t i) {
                    const uint32_t p = (i * 97u + static_cast<uint32_t>(ctx.frame)) % kN;
                    return dm::pow(1.0f + dm::sqrt(x[p] * x[p]) * 0.01f, 0.5f);  // pure score
                },
                [this](uint32_t i, float score) {
                    alamo::SimRng& r = rng.synced();  // draws only in the ordered commit
                    if (r.percent(static_cast<int32_t>(score * 10.0f))) events.push_back(i * 1000u + static_cast<uint32_t>(r.range_int(0, 999)));
                });
        });
    }
};

alamo::CrcLog run(int32_t workers, uint64_t ticks, bool parallel_capture, int64_t corrupt_tick = -1) {
    JobSystem js(JobSystemConfig{workers});
    ToySim sim(js);
    TickScheduler sched(js);
    sim.add_systems(sched, js);
    if (corrupt_tick >= 0)
        sched.add_system("corrupt", 1, 0, [&sim, corrupt_tick](const TickContext& ctx) {
            if (ctx.frame == static_cast<uint64_t>(corrupt_tick)) sim.vx[123] += 1e-6f;
        });
    CrcRecorder rec(js, sim.reg, parallel_capture);
    for (uint64_t i = 0; i < ticks; ++i) {
        const uint64_t frame = sched.frame();
        sched.step();
        rec.capture(frame);
    }
    return rec.log();
}

}  // namespace

int main() {
    const uint64_t kTicks = 240;
    const auto counts = determinism_worker_counts();
    CHECK(counts.size() >= 3 && counts[0] == 0 && counts[1] == 1 && counts[2] == 4);
    const alamo::CrcLog ref = run(0, kTicks, false);
    CHECK(ref.records.size() == kTicks && ref.systems.size() == 4);
    CHECK(ref.records.front().tick == 0 && ref.records.back().tick == kTicks - 1);
    for (bool pc : {false, true}) {  // OPT3 t07 helper: every count against the first (0 workers)
        const WorkerCountReport rep = check_worker_counts([&](int32_t w) { return run(w, kTicks, pc); }, counts);
        for (const WorkerCountRun& r : rep.runs)
            std::printf("  workers %2d capture %-8s: %s\n", r.workers, pc ? "parallel" : "serial", r.vs_first.message.c_str());
        CHECK(rep.ok() && rep.runs.size() == counts.size() && rep.runs.front().workers == 0);
        CHECK(!alamo::first_divergence(ref, rep.first).diverged);
    }
    // The state actually changes every tick (else the check proves nothing).
    CHECK(ref.records[10].systems[0] != ref.records[11].systems[0]);
    CHECK(ref.records[100].systems[2] != ref.records[0].systems[2]);  // events were drawn
    // Injected divergence: one ulp-scale velocity change at tick 50 under 4 workers.
    const alamo::CrcLog bad = run(4, kTicks, false, 50);
    const auto d = alamo::first_divergence(ref, bad);
    std::printf("  injected: %s\n", d.message.c_str());
    CHECK(d.diverged && d.tick == 50 && d.system == "particles");
    // The helper reports the divergence on the count that has it, and only there.
    const WorkerCountReport bad_rep =
        check_worker_counts([&](int32_t w) { return run(w, kTicks, false, w == 4 ? 50 : -1); }, {0, 1, 4});
    CHECK(!bad_rep.ok() && bad_rep.runs.size() == 3);
    CHECK(!bad_rep.runs[0].vs_first.diverged && !bad_rep.runs[1].vs_first.diverged);
    CHECK(bad_rep.runs[2].vs_first.diverged && bad_rep.runs[2].vs_first.tick == 50 &&
          bad_rep.runs[2].vs_first.system == "particles");
    // cu9 t18: the overload with a caller-run first result and an extra comparison (alamo_headless's --check-workers).
    {
        struct Res {
            alamo::CrcLog log;
            int32_t extra = 0;
        };
        const auto log_of = [](const Res& r) -> const alamo::CrcLog& { return r.log; };
        const Res first{run(0, kTicks, false), 7};
        std::vector<int32_t> seen;
        // CRC logs match, the extra data matches: ok, every further count reported once, in order.
        CHECK(check_worker_counts(
            first, [&](int32_t w) { return Res{run(w, kTicks, false), 7}; }, log_of,
            [&](int32_t w, const Res& r, const alamo::CrcDivergence& dv) {
                seen.push_back(w);
                return !dv.diverged && r.extra == first.extra;
            },
            {0, 1, 4}));
        CHECK((seen == std::vector<int32_t>{1, 4}));
        // The extra data differs on one count: not ok, although the CRC logs match.
        CHECK(!check_worker_counts(
            first, [&](int32_t w) { return Res{run(w, kTicks, false), w == 4 ? 8 : 7}; }, log_of,
            [&](int32_t, const Res& r, const alamo::CrcDivergence&) { return r.extra == first.extra; }, {0, 1, 4}));
        // The CRC log diverges while the report says the extra data matches: still not ok.
        CHECK(!check_worker_counts(
            first, [&](int32_t w) { return Res{run(w, kTicks, false, w == 1 ? 50 : -1), 7}; }, log_of,
            [&](int32_t, const Res&, const alamo::CrcDivergence&) { return true; }, {0, 1}));
    }
    // Text round trip of a real log.
    alamo::CrcLog back;
    CHECK(alamo::CrcLog::parse(ref.to_text(), back) && !alamo::first_divergence(ref, back).diverged);

    std::printf(g_fail ? "alamo_jobs_crc_tests: %d FAILED\n" : "alamo_jobs_crc_tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
