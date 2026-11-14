// alamo_jobs unit tests (no framework; returns non-zero on failure).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "alamo/jobs/jobs.hpp"
#include "alamo/jobs/topology.hpp"
#include "alamo/test/support.hpp"

using namespace alamo::jobs;

#if defined(__SANITIZE_THREAD__)
#define ALAMO_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ALAMO_TSAN 1
#endif
#endif
#ifdef ALAMO_TSAN
constexpr uint32_t kScale = 10;  // TSan is ~10-20x slower
#else
constexpr uint32_t kScale = 1;
#endif

using ::alamo::test::g_fail;

static const std::array<int32_t, 5> kWorkerCounts = {0, 1, 2, 4, 16};

static void section(const char* name) {
    std::printf("-- %s\n", name);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------

static void test_config() {
    section("config");
    CHECK(JobSystem::default_worker_count() >= 1);
    {
        JobSystem js;
        CHECK(js.worker_count() == JobSystem::default_worker_count());
        CHECK(js.current_slot() == 0);
    }
    {
        JobSystem js({0});
        CHECK(js.serial() && js.thread_count() == 1);
    }
    {
        JobSystem js({3});
        CHECK(js.worker_count() == 3 && js.executed_per_slot().size() == 4);
        // Slot ids seen from inside jobs are 0..3.
        std::vector<std::atomic<uint32_t>> seen(4);
        js.parallel_for(10000, 1, [&](uint32_t, uint32_t) {
            const uint32_t s = js.current_slot();
            if (s < 4) seen[s].fetch_add(1);
            std::this_thread::yield();
        });
        uint32_t total = 0;
        for (auto& s : seen) total += s.load();
        CHECK(total == 10000);
    }
}

static void test_stress() {
    section("stress: millions of tiny jobs");
    for (int32_t w : {0, 1, 4}) {
        JobSystem js({w});
        // parallel_for, grain 1: every index exactly once.
        const uint32_t n = 2'000'000 / kScale;
        std::vector<uint8_t> hit(n, 0);
        js.parallel_for(n, 1, [&](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) ++hit[i];
        });
        bool ok = true;
        for (uint8_t h : hit) ok &= (h == 1);
        CHECK(ok);

        // Raw submit: 1M (scaled) jobs into one counter.
        const uint32_t m = 1'000'000 / kScale;
        std::atomic<uint64_t> sum{0};
        JobCounter c;
        for (uint32_t i = 0; i < m; ++i)
            js.submit(c, [](void* ctx, uint32_t a, uint32_t) {
                static_cast<std::atomic<uint64_t>*>(ctx)->fetch_add(a, std::memory_order_relaxed);
            }, &sum, i);
        js.wait(c);
        CHECK(c.done());
        CHECK(sum.load() == static_cast<uint64_t>(m) * (m - 1) / 2);

        // Many small parallel_for calls back to back (call overhead path).
        uint64_t total = 0;
        std::vector<uint32_t> out(64);
        for (uint32_t rep = 0; rep < 20000 / kScale; ++rep) {
            js.parallel_for(64, 4, [&](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) out[i] = i + rep;
            });
            for (uint32_t v : out) total += v;
        }
        uint64_t expect = 0;
        for (uint32_t rep = 0; rep < 20000 / kScale; ++rep)
            for (uint32_t i = 0; i < 64; ++i) expect += i + rep;
        CHECK(total == expect);
    }
    // Work actually spreads across threads.
    {
        JobSystem js({2});
        // Sleeping chunks: robust even when worker threads are slow to get scheduled.
        js.parallel_for(64, 1, [](uint32_t, uint32_t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        const auto per = js.executed_per_slot();
        CHECK(per[1] + per[2] > 0);  // helpers ran on workers
    }
    // Concurrent external submitters into one pool.
    {
        JobSystem js({3});
        std::atomic<uint64_t> sum{0};
        std::vector<std::thread> ext;
        for (int t = 0; t < 3; ++t)
            ext.emplace_back([&] {
                for (uint32_t r = 0; r < 200 / kScale + 1; ++r) {
                    js.parallel_for(1000, 7, [&](uint32_t b, uint32_t e) {
                        sum.fetch_add(e - b, std::memory_order_relaxed);
                    });
                }
            });
        for (auto& t : ext) t.join();
        CHECK(sum.load() == 3ull * (200 / kScale + 1) * 1000);
    }
}

static void test_nested() {
    section("nested parallel_for");
    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        constexpr uint32_t A = 16, B = 16, C = 64;
        std::vector<uint32_t> cell(A * B * C, 0);
        js.parallel_for(A, 1, [&](uint32_t ab, uint32_t ae) {
            for (uint32_t a = ab; a < ae; ++a)
                js.parallel_for(B, 1, [&](uint32_t bb, uint32_t be) {
                    for (uint32_t b = bb; b < be; ++b)
                        js.parallel_for(C, 3, [&](uint32_t cb, uint32_t ce) {
                            for (uint32_t c = cb; c < ce; ++c) cell[(a * B + b) * C + c] += a + b + c;
                        });
                });
        });
        bool ok = true;
        for (uint32_t a = 0; a < A; ++a)
            for (uint32_t b = 0; b < B; ++b)
                for (uint32_t c = 0; c < C; ++c) ok &= cell[(a * B + b) * C + c] == a + b + c;
        CHECK(ok);

        // parallel_for + wait from inside spawned jobs, every worker blocked in a wait.
        JobCounter outer;
        std::atomic<uint32_t> inner_sum{0};
        for (uint32_t j = 0; j < 32; ++j)
            js.spawn(outer, [&] {
                JobCounter inner;
                for (uint32_t k = 0; k < 8; ++k) js.spawn(inner, [&] { inner_sum.fetch_add(1); });
                js.parallel_for(100, 1, [&](uint32_t b, uint32_t e) { inner_sum.fetch_add(e - b); });
                js.wait(inner);
            });
        js.wait(outer);
        CHECK(inner_sum.load() == 32u * 108u);
    }
}

static void test_exceptions() {
    section("exceptions");
    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        // parallel_for rethrows the lowest failing chunk, same as serial.
        for (int rep = 0; rep < 20; ++rep) {
            std::string what;
            try {
                js.parallel_for(1000, 10, [](uint32_t b, uint32_t) {
                    if (b == 170 || b == 50 || b == 900) throw std::runtime_error("chunk " + std::to_string(b));
                });
            } catch (const std::runtime_error& e) {
                what = e.what();
            }
            CHECK(what == "chunk 50");
        }
        // spawn/submit: rethrown at wait, lowest key wins.
        JobCounter c;
        for (uint32_t i = 0; i < 100; ++i)
            js.submit(c, [](void*, uint32_t a, uint32_t) {
                if (a % 30 == 7) throw std::runtime_error("job " + std::to_string(a));
            }, nullptr, i);
        std::string what;
        try {
            js.wait(c);
        } catch (const std::runtime_error& e) {
            what = e.what();
        }
        CHECK(what == "job 7");
        CHECK(c.done());
        // Counter reusable, system still healthy.
        std::atomic<int> ok{0};
        js.spawn(c, [&] { ok = 1; });
        js.wait(c);
        CHECK(ok.load() == 1);
        // Exception from a nested parallel_for propagates through the outer one.
        bool caught = false;
        try {
            js.parallel_for(8, 1, [&](uint32_t b, uint32_t) {
                js.parallel_for(8, 1, [&](uint32_t ib, uint32_t) {
                    if (b == 3 && ib == 5) throw std::logic_error("inner");
                });
            });
        } catch (const std::logic_error&) {
            caught = true;
        }
        CHECK(caught);
    }
}

static void test_serial_mode() {
    section("0-worker serial mode");
    JobSystem js({0});
    const auto me = std::this_thread::get_id();
    std::vector<uint32_t> order;
    bool same_thread = true;
    js.parallel_for(100, 7, [&](uint32_t b, uint32_t e) {
        same_thread &= std::this_thread::get_id() == me;
        order.push_back(b);
        CHECK(e - b <= 7);
    });
    CHECK(same_thread);
    bool ascending = order.size() == 15;
    for (size_t i = 0; i < order.size(); ++i) ascending &= order[i] == i * 7;
    CHECK(ascending);
    // Submitted jobs run inside wait(), on the waiting thread.
    JobCounter c;
    int ran = 0;
    for (int i = 0; i < 10; ++i)
        js.spawn(c, [&] {
            same_thread &= std::this_thread::get_id() == me;
            ++ran;
        });
    CHECK(ran == 0);
    js.wait(c);
    CHECK(ran == 10 && same_thread);
    // Background jobs too.
    js.spawn(c, [&] { ++ran; }, Priority::Background);
    js.wait(c);
    CHECK(ran == 11);
}

static float weird_value(uint32_t i) {
    // Wide magnitude range so the summation order matters at the bit level.
    const float m = static_cast<float>((i * 2654435761u) % 1000u) / 7.0f;
    const int e = static_cast<int>(i % 40) - 20;
    return std::ldexp(m, e) * ((i & 1) ? -1.0f : 1.0f);
}

static void test_reduce_determinism() {
    section("parallel_reduce determinism");
    const uint32_t n = 1'000'003;
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = weird_value(i);
    auto map = [&](uint32_t b, uint32_t e) {
        float s = 0.0f;
        for (uint32_t i = b; i < e; ++i) s += v[i];
        return s;
    };
    auto add = [](float a, float b) { return a + b; };

    // Reference: the same chunked fold computed serially.
    float ref = 0.0f;
    for (uint32_t b = 0; b < n; b += 777) ref = ref + map(b, std::min(n, b + 777));
    float naive = 0.0f;
    for (float x : v) naive += x;

    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        for (int rep = 0; rep < 5; ++rep) {
            const float r = parallel_reduce(js, n, 777, 0.0f, map, add);
            CHECK(std::memcmp(&r, &ref, sizeof r) == 0);
        }
        // Through core's ParallelFor hook.
        const alamo::ParallelFor pf = make_parallel_for(js);
        const float r2 = parallel_reduce(pf, n, 777, 0.0f, map, add);
        CHECK(std::memcmp(&r2, &ref, sizeof r2) == 0);
    }
    const alamo::ParallelFor serial = alamo::serial_for;
    const float r3 = parallel_reduce(serial, n, 777, 0.0f, map, add);
    CHECK(std::memcmp(&r3, &ref, sizeof r3) == 0);
    std::printf("   chunked=%a naive=%a (differ: %s - order matters, chunking fixes it)\n", ref, naive,
                std::memcmp(&ref, &naive, sizeof ref) ? "yes" : "no");

    // Non-trivial T (vector of per-bucket sums), still ordered.
    JobSystem js({4});
    auto hist = parallel_reduce(
        js, n, 4096, std::vector<double>(8, 0.0),
        [&](uint32_t b, uint32_t e) {
            std::vector<double> h(8, 0.0);
            for (uint32_t i = b; i < e; ++i) h[i % 8] += v[i];
            return h;
        },
        [](std::vector<double> a, std::vector<double> b) {
            for (size_t k = 0; k < a.size(); ++k) a[k] += b[k];
            return a;
        });
    CHECK(hist.size() == 8);
}

static void test_map_commit() {
    section("parallel_map_then_ordered_commit");
    // AI goal proposal shape: pure score per (goal,target), serial insert of positive scores in
    // candidate order; the insert also draws from an RNG, so order must equal the serial loop.
    const uint32_t n = 50'000;
    auto score = [](uint32_t i) {
        const float x = std::sin(static_cast<float>(i) * 0.37f) * 10.0f;
        return x;
    };
    auto run = [&](auto& exec) {
        std::vector<std::pair<uint32_t, float>> proposed;
        uint32_t rng = 12345;
        parallel_map_then_ordered_commit(exec, n, 64, score, [&](uint32_t i, float s) {
            if (s > 0.0f) {
                rng = rng * 1103515245u + 12345u;
                proposed.emplace_back(i ^ (rng >> 16), s);
            }
        });
        return std::make_pair(proposed, rng);
    };
    JobSystem serial({0});
    const auto ref = run(serial);
    CHECK(!ref.first.empty());
    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        const auto got = run(js);
        CHECK(got == ref);
        const alamo::ParallelFor pf = make_parallel_for(js);
        CHECK(run(pf) == ref);
    }
    // Commit runs on the caller, in index order.
    JobSystem js({4});
    std::vector<uint32_t> seq;
    CommitBuffer<int> buffer;
    const auto me = std::this_thread::get_id();
    bool on_caller = true;
    parallel_map_then_ordered_commit(
        js, 1000, 1, [](uint32_t i) { return static_cast<int>(i) * 2; },
        [&](uint32_t i, int v) {
            on_caller &= std::this_thread::get_id() == me;
            CHECK(v == static_cast<int>(i) * 2);
            seq.push_back(i);
        },
        buffer);
    bool ordered = seq.size() == 1000;
    for (uint32_t i = 0; i < seq.size(); ++i) ordered &= seq[i] == i;
    CHECK(ordered && on_caller && buffer.size() == 1000);
}

static void test_bool_results() {
    section("bool per-index results (no vector<bool> packing race)");
    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        int bad = 0;
        for (int rep = 0; rep < 100 / static_cast<int>(kScale) + 5; ++rep) {
            CommitBuffer<bool> buf;
            uint32_t ones = 0;
            parallel_map_then_ordered_commit(
                js, 4096, 1, [](uint32_t i) { return (i % 3) != 0; },
                [&](uint32_t i, bool v) {
                    if (v != ((i % 3) != 0)) ++bad;
                    ones += v ? 1u : 0u;
                },
                buf);
            if (ones != 4096 - 1366) ++bad;
            // map returning bool through the default-buffer overload
            uint32_t ones2 = 0;
            parallel_map_then_ordered_commit(js, 999, 2, [](uint32_t i) { return i % 2 == 0; },
                                             [&](uint32_t, bool v) { ones2 += v ? 1u : 0u; });
            if (ones2 != 500) ++bad;
            const bool r = parallel_reduce<bool>(
                js, 4096, 1, false, [](uint32_t b, uint32_t) { return b == 4000; },
                [](bool a, bool x) { return a || x; });
            if (!r) ++bad;
        }
        CHECK(bad == 0);
    }
}

static void test_tick_rate() {
    section("tick rate");
    JobSystem js({0});
    CHECK(TickScheduler(js).ticks_per_second() == kDefaultTicksPerSecond);
    CHECK(std::fabs(TickScheduler(js, 20).fixed_dt() - 0.05) < 1e-12);
    CHECK(TickScheduler(js, 0).ticks_per_second() == 1);  // clamped
}

static void test_tick_cadence() {
    section("TickScheduler cadence");
    for (int32_t w : {0, 2}) {
        JobSystem js({w});
        TickScheduler ts(js);
        std::vector<std::pair<uint64_t, int>> log;
        ts.add_system("every", 1, 0, [&](const TickContext& c) { log.emplace_back(c.frame, 0); });
        ts.add_system("p90", 90, 0, [&](const TickContext& c) { log.emplace_back(c.frame, 1); });
        ts.add_system("p90ph45", 90, 45, [&](const TickContext& c) { log.emplace_back(c.frame, 2); });
        ts.add_system("p7ph10", 7, 10, [&](const TickContext& c) { log.emplace_back(c.frame, 3); });  // phase 3
        const auto pf = ts.add_system("par", 5, 1, [&](const TickContext& c) {
            uint64_t s = 0;
            std::vector<uint64_t> tmp(100);
            c.jobs.parallel_for(100, 10, [&](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) tmp[i] = c.frame + i;
            });
            for (auto v : tmp) s += v;
            log.emplace_back(c.frame, 4 + static_cast<int>(s == 100 * c.frame + 4950 ? 0 : 100));
        });
        for (int i = 0; i < 200; ++i) ts.step();
        CHECK(ts.frame() == 200);
        std::vector<std::pair<uint64_t, int>> expect;
        for (uint64_t f = 0; f < 200; ++f) {
            expect.emplace_back(f, 0);
            if (f % 90 == 0) expect.emplace_back(f, 1);
            if (f % 90 == 45) expect.emplace_back(f, 2);
            if (f % 7 == 3) expect.emplace_back(f, 3);
            if (f % 5 == 1) expect.emplace_back(f, 4);
        }
        CHECK(log == expect);
        ts.set_enabled(pf, false);
        log.clear();
        for (int i = 0; i < 10; ++i) ts.step();
        bool none = true;
        for (auto& [f, id] : log) none &= id != 4;
        CHECK(none && ts.frame() == 210);
        bool threw = false;
        try {
            ts.add_system("bad", 0, 0, [](const TickContext&) {});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
}

static void test_deferred() {
    section("Deferred results");
    for (int32_t w : kWorkerCounts) {
        JobSystem js({w});
        // Early finish: result available long before T+k but not visible until T+k.
        {
            Deferred<int> d(js, -1);
            d.kick(10, 5, [] { return 42; });
            while (!d.computed()) std::this_thread::yield();
            for (uint64_t f = 10; f < 15; ++f) CHECK(d.read(f) == -1);
            CHECK(d.in_flight());
            CHECK(d.read(15) == 42 && !d.in_flight() && d.version() == 1);
            CHECK(d.read(100) == 42);
        }
        // Late finish: reader at T+k blocks until the job completes.
        {
            Deferred<int> d(js, 0);
            std::atomic<bool> release{false};
            const auto t0 = std::chrono::steady_clock::now();
            d.kick(0, 3, [&] {
                // Serial mode runs at kick: don't wait for a release that only comes later.
                if (!js.serial()) {
                    while (!release.load()) std::this_thread::yield();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
                return 7;
            });
            CHECK(d.read(1) == 0 && d.read(2) == 0);
            release = true;
            CHECK(d.read(3) == 7);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            if (!js.serial()) CHECK(ms >= 25);
        }
        // Double buffering with in-place compute (back buffer reuse) and repeated kicks.
        {
            Deferred<std::vector<int>> d(js, std::vector<int>(4, 0));
            for (int gen = 1; gen <= 5; ++gen) {
                const uint64_t t = static_cast<uint64_t>(gen) * 90;
                d.kick(t, 90, [gen](std::vector<int>& out) { out.assign(4, gen); });
                CHECK(d.read(t) == std::vector<int>(4, gen - 1));  // previous result until T+k
                CHECK(d.read(t + 89) == std::vector<int>(4, gen - 1));
                CHECK(d.read(t + 90) == std::vector<int>(4, gen));
            }
        }
        // Kick while previous result not yet due -> logic_error; exception in compute -> at T+k.
        {
            Deferred<int> d(js, 1);
            d.kick(0, 10, [] { return 2; });
            bool threw = false;
            try {
                d.kick(5, 10, [] { return 3; });
            } catch (const std::logic_error&) {
                threw = true;
            }
            CHECK(threw);
            d.kick(10, 1, [] { return 4; });  // publishes the first (due at 10) then kicks
            CHECK(d.current() == 2 && d.read(11) == 4);
            d.kick(20, 2, []() -> int { throw std::runtime_error("boom"); });
            CHECK(d.read(21) == 4);
            std::string what;
            try {
                d.read(22);
            } catch (const std::runtime_error& e) {
                what = e.what();
            }
            CHECK(what == "boom" && d.current() == 4 && !d.in_flight());
        }
        // Lockstep scenario: AI system with a 90-frame cadence kicks its recompute; readers every
        // frame. Identical observation log for every worker count (compared against serial).
        {
            TickScheduler ts(js);
            Deferred<uint64_t> allpairs(js, 0);
            ts.track(allpairs);
            std::vector<uint64_t> observed;
            ts.add_system("ai_recompute", 90, 17, [&](const TickContext& c) {
                const uint64_t snapshot = c.frame;  // inputs captured by value at kick
                allpairs.kick(c.frame, 30, [snapshot] {
                    uint64_t h = snapshot;
                    for (int i = 0; i < 20000; ++i) h = h * 6364136223846793005ull + 1442695040888963407ull;
                    return h;
                });
            });
            ts.add_system("ai_read", 1, 0, [&](const TickContext& c) { observed.push_back(allpairs.read(c.frame)); });
            for (int i = 0; i < 400; ++i) ts.step();
            static std::vector<uint64_t> reference;
            if (w == 0) reference = observed;
            CHECK(observed == reference);
            // Visible exactly from frame 17+30=47, 137, 227, 317.
            CHECK(observed[46] == 0 && observed[47] != 0 && observed[136] == observed[47] &&
                  observed[137] != observed[136]);
        }
    }
}

static void test_shutdown() {
    section("shutdown with pending work");
    for (int32_t w : {0, 1, 4}) {
        std::atomic<uint32_t> ran{0};
        JobCounter c;  // outlives the system
        {
            JobSystem js({w});
            for (uint32_t i = 0; i < 20000 / kScale; ++i)
                js.spawn(c, [&] { ran.fetch_add(1, std::memory_order_relaxed); });
            for (uint32_t i = 0; i < 100; ++i)
                js.spawn(c, [&] { ran.fetch_add(1, std::memory_order_relaxed); }, Priority::Background);
            // Jobs that spawn more jobs while the destructor is draining.
            for (uint32_t i = 0; i < 50; ++i)
                js.spawn(c, [&] {
                    for (int k = 0; k < 10; ++k) js.spawn(c, [&] { ran.fetch_add(1); });
                });
        }  // ~JobSystem drains everything
        CHECK(ran.load() == 20000 / kScale + 100 + 500);
        CHECK(c.done());
    }
    // Repeated create/destroy with idle workers.
    for (int i = 0; i < 50; ++i) {
        JobSystem js({3});
        if (i % 2) js.parallel_for(100, 1, [](uint32_t, uint32_t) {});
    }
}

static void test_background_priority() {
    section("background priority");
    JobSystem js({1});
    // A long background job must not be picked up by the sim thread's parallel_for wait.
    std::atomic<bool> release{false};
    std::atomic<bool> bg_started{false};
    std::thread::id bg_thread;
    JobCounter bg;
    js.spawn(bg, [&] {
        bg_thread = std::this_thread::get_id();
        bg_started = true;
        while (!release.load()) std::this_thread::yield();
    }, Priority::Background);
    while (!bg_started.load()) std::this_thread::yield();
    // Worker is busy: the caller runs the whole parallel_for and returns without blocking on bg.
    std::atomic<uint32_t> sum{0};
    js.parallel_for(1000, 1, [&](uint32_t b, uint32_t e) { sum.fetch_add(e - b); });
    CHECK(sum.load() == 1000);
    release = true;
    js.wait(bg);
    CHECK(bg_thread != std::this_thread::get_id());

    // wait(c) claims c's own background job if no worker has started it.
    JobCounter block, mine;
    std::atomic<bool> go{false};
    js.spawn(block, [&] { while (!go.load()) std::this_thread::yield(); }, Priority::Background);
    std::thread::id ran_on;
    js.spawn(mine, [&] { ran_on = std::this_thread::get_id(); }, Priority::Background);
    // Give the worker time to take `block` (FIFO), then wait on `mine` while the worker is stuck.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    js.wait(mine);
    CHECK(ran_on == std::this_thread::get_id());
    go = true;
    js.wait(block);
}

static void test_background_isolation() {
    section("background isolation (inherited priority)");
    JobSystem js({2});
    const auto main_id = std::this_thread::get_id();
    std::atomic<int> main_ran_bg{0};
    std::atomic<bool> bg_inner_started{false};
    JobCounter bg;
    // A background job whose own parallel_for and nested spawns must stay background.
    js.spawn(bg, [&] {
        JobCounter inner;
        js.spawn(inner, [&] {
            if (std::this_thread::get_id() == main_id) main_ran_bg++;
        });
        js.parallel_for(200 / kScale + 20, 1, [&](uint32_t, uint32_t) {
            bg_inner_started = true;
            if (std::this_thread::get_id() == main_id) main_ran_bg++;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        });
        js.wait(inner);
    }, Priority::Background);
    while (!bg_inner_started.load()) std::this_thread::yield();
    // Sim thread: short parallel_fors and a deferred read while bg is busy.
    std::atomic<int> nodes{0};
    for (int frame = 0; frame < 20; ++frame) {
        std::atomic<uint32_t> s{0};
        js.parallel_for(16, 1, [&](uint32_t b, uint32_t e) { s += e - b; });
        CHECK(s.load() == 16);
        js.parallel_for(8, 1, [&](uint32_t b, uint32_t e) { nodes += static_cast<int>(e - b); });
    }
    Deferred<int> d(js, 0);
    d.kick(0, 1, [] { return 5; });
    CHECK(d.read(1) == 5);
    js.wait(bg);
    CHECK(nodes.load() == 160);
    CHECK(main_ran_bg.load() == 0);

    // A sleeping waiter runs its OWN background job even if it is queued after it went to
    // sleep, while the only worker is stuck waiting for that job (would hang otherwise).
    JobSystem one({1});
    JobCounter c;
    std::atomic<bool> a_started{false}, b_ran{false};
    std::thread::id b_thread;
    one.spawn(c, [&] {
        a_started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let main fall asleep
        one.spawn(c, [&] {
            b_thread = std::this_thread::get_id();
            b_ran = true;
        }, Priority::Background);
        while (!b_ran.load()) std::this_thread::yield();
    });
    while (!a_started.load()) std::this_thread::yield();
    one.wait(c);
    CHECK(b_ran.load() && b_thread == main_id);
}

// Frame-time scaling (2026-11-14): parallel_for waits for chunks, not helper jobs.
static void test_for_waits_on_chunks() {
    section("parallel_for: caller does not wait for unstarted helpers");
    {
        // Both workers busy: the caller runs every chunk itself, takes its queued helpers back
        // and returns without the workers ever running them.
        JobSystem js({2});
        std::atomic<bool> release{false};
        std::atomic<uint32_t> started{0};
        JobCounter busy;
        for (int i = 0; i < 2; ++i)
            js.spawn(busy, [&] {
                started.fetch_add(1);
                while (!release.load()) std::this_thread::yield();
            });
        while (started.load() < 2) std::this_thread::yield();
        const auto before = js.executed_per_slot();
        std::atomic<uint32_t> sum{0};
        js.parallel_for(100, 1, [&](uint32_t b, uint32_t e) { sum.fetch_add(e - b); });
        CHECK(sum.load() == 100);
        release = true;
        js.wait(busy);
        // Give the workers a moment: no stale helper may be left for them.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto after = js.executed_per_slot();
        CHECK(after[1] + after[2] - before[1] - before[2] == 2);  // just the two busy jobs
        CHECK(after[0] == before[0]);                            // the caller ran no helper job either
    }
    {
        // Lifetime: many short loops from several threads, nested loops and failures; late helpers
        // must only ever see their own (still alive) state. ASan / the sums catch a mistake.
        JobSystem js({4});
        std::atomic<uint64_t> sum{0};
        std::vector<std::thread> ext;
        for (int t = 0; t < 3; ++t)
            ext.emplace_back([&] {
                for (uint32_t r = 0; r < 3000 / kScale + 1; ++r) {
                    js.parallel_for(6, 1, [&](uint32_t b, uint32_t e) {
                        js.parallel_for(3, 1, [&](uint32_t b2, uint32_t e2) { sum.fetch_add(e2 - b2); });
                        sum.fetch_add(e - b);
                    });
                    try {
                        js.parallel_for(8, 1, [](uint32_t b, uint32_t) {
                            if (b == 5) throw std::runtime_error("x");
                        });
                    } catch (const std::runtime_error&) {
                    }
                }
            });
        for (auto& t : ext) t.join();
        CHECK(sum.load() == 3ull * (3000 / kScale + 1) * (6 + 6 * 3));
    }
}

static void test_background_cap() {
    section("background cap");
    JobSystem js({4});
    CHECK(js.background_cap() == 2);
    CHECK(JobSystem({1}).background_cap() == 1);
    CHECK(JobSystem({0}).background_cap() == 0);
    std::atomic<bool> release{false};
    std::atomic<uint32_t> running{0}, peak{0};
    JobCounter bg;
    for (int i = 0; i < 6; ++i)
        js.spawn(bg, [&] {
            const uint32_t now = running.fetch_add(1) + 1;
            uint32_t p = peak.load();
            while (now > p && !peak.compare_exchange_weak(p, now)) {}
            while (!release.load()) std::this_thread::yield();
            running.fetch_sub(1);
        }, Priority::Background);
    for (int i = 0; i < 2000 && running.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a third would have started by now
    CHECK(peak.load() == 2);
    // Two workers stay free for foreground work while the cap is reached.
    const auto before = js.executed_per_slot();
    js.parallel_for(64, 1, [](uint32_t, uint32_t) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    const auto after = js.executed_per_slot();
    uint64_t helped = 0;
    for (size_t s = 1; s < after.size(); ++s) helped += after[s] - before[s];
    CHECK(helped > 0);
    release = true;
    js.wait(bg);  // the queued ones run once the first two finish (the waiter may claim them too)
    CHECK(running.load() == 0);

    // Cap 1, full: a Deferred result due now is claimed and run by the waiting sim thread.
    {
        JobSystem two({2});
        CHECK(two.background_cap() == 1);
        std::atomic<bool> go{false}, blocker_started{false};
        JobCounter blocker;
        two.spawn(blocker, [&] {
            blocker_started = true;
            while (!go.load()) std::this_thread::yield();
        }, Priority::Background);
        while (!blocker_started.load()) std::this_thread::yield();
        Deferred<int> d(two, 0);
        d.kick(0, 1, [] { return 7; });
        CHECK(d.read(1) == 7);  // would hang if the cap kept the job from every thread
        go = true;
        two.wait(blocker);
    }
    // Cap 1, used by a background job: the helpers of its own parallel_for are exempt, so the
    // other (idle) worker joins the loop instead of the job running it alone.
    {
        JobSystem two({2});
        std::mutex m;
        std::vector<std::thread::id> ran_on;
        JobCounter c;
        two.spawn(c, [&] {
            two.parallel_for(64, 1, [&](uint32_t, uint32_t) {
                {
                    std::lock_guard lk(m);
                    if (std::find(ran_on.begin(), ran_on.end(), std::this_thread::get_id()) == ran_on.end())
                        ran_on.push_back(std::this_thread::get_id());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        }, Priority::Background);
        two.wait(c);
        CHECK(ran_on.size() >= 2);
    }
}

static void test_abort_skips_later_chunks() {
    section("exception stops later chunks");
    for (int32_t w : {0, 1, 4}) {
        JobSystem js({w});
        std::atomic<uint32_t> ran{0};
        bool caught = false;
        try {
            js.parallel_for(100000, 1, [&](uint32_t b, uint32_t) {
                ran++;
                if (b == 0) throw std::runtime_error("first");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught && ran.load() < 50000);
    }
}

static void test_scratch() {
    section("scratch arena");
    ScratchArena a(4096);
    const auto m0 = a.mark();
    void* p1 = a.alloc(10, 1);
    void* p2 = a.alloc(16, 64);
    CHECK(reinterpret_cast<uintptr_t>(p2) % 64 == 0);
    CHECK(p1 != p2);
    auto* big = a.alloc_array<double>(10000);  // forces a new block
    big[9999] = 1.0;
    CHECK(a.block_count() >= 2);
    const auto m1 = a.mark();
    a.alloc(100);
    a.rewind(m1);
    CHECK(a.mark().block == m1.block && a.mark().offset == m1.offset);
    a.rewind(m0);  // back to empty -> coalesced into one block
    CHECK(a.block_count() == 1 && a.capacity() >= 4096 + 80000 && a.bytes_in_use() == 0);
    void* p3 = a.alloc(80000, 8);  // fits in the coalesced block now
    CHECK(p3 != nullptr && a.block_count() == 1);
    a.reset();
    {
        ScratchScope s(a);
        a.alloc(1000);
        {
            ScratchScope inner(a);
            a.alloc(1000);
        }
        CHECK(a.bytes_in_use() >= 1000 && a.bytes_in_use() < 2000);
    }
    CHECK(a.bytes_in_use() == 0);

    // Serial-mode Deferred::kick runs its compute inside a scratch scope.
    {
        JobSystem serial({0});
        Deferred<int> d(serial, 0);
        d.kick(0, 0, [] {
            thread_scratch().alloc(5000);
            return 1;
        });
        CHECK(d.read(0) == 1 && thread_scratch().bytes_in_use() == 0);
    }

    // pmr containers on the thread arena inside parallel jobs (perception context shape).
    JobSystem js({4});
    std::vector<int64_t> out(2000);
    js.parallel_for(2000, 8, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            ScratchScope scope;
            std::pmr::unordered_map<int, int64_t> tokens(&thread_scratch());
            for (int k = 0; k < 8; ++k) tokens.emplace(k, static_cast<int64_t>(i) * k);
            int64_t s = 0;
            for (int k = 0; k < 8; ++k) s += tokens.at(k);
            out[i] = s;
        }
    });
    bool ok = true;
    for (uint32_t i = 0; i < 2000; ++i) ok &= out[i] == static_cast<int64_t>(i) * 28;
    CHECK(ok);
    // Jobs are scoped automatically: thread arena empty again on this thread.
    CHECK(thread_scratch().bytes_in_use() == 0);
}

// sp1: strict priorities, critical work between chunks of an AI loop, inline small forks, pool plan.
static void busy_us(int us) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {
    }
}

static void test_priority_order() {
    section("sp1: critical > AI > background");
    JobSystem js({1});
    std::atomic<bool> release{false}, started{false};
    JobCounter block;
    js.spawn(block, [&] {
        started = true;
        while (!release.load()) std::this_thread::yield();
    });
    while (!started.load()) std::this_thread::yield();
    std::mutex m;
    std::vector<char> order;
    auto rec = [&](char c) {
        std::lock_guard lk(m);
        order.push_back(c);
    };
    JobCounter all;
    js.spawn(all, [&] { rec('B'); }, Priority::Background);
    js.spawn(all, [&] { rec('A'); }, Priority::Ai);
    js.spawn(all, [&] { rec('C'); }, Priority::Critical);
    // The main thread only polls (a wait would help with C, or claim its own A / B), so the worker alone picks the order.
    release = true;
    while (!block.done() || !all.done()) std::this_thread::yield();
    CHECK(order.size() == 3);
    if (order.size() == 3) CHECK(order[0] == 'C' && order[1] == 'A' && order[2] == 'B');

    // A critical waiter never runs an AI job; an AI thread (ScopedPriority) runs AI work while it waits.
    std::atomic<bool> go{false}, busy_started{false};
    JobCounter busy;
    js.spawn(busy, [&] {
        busy_started = true;
        while (!go.load()) std::this_thread::yield();
    });
    while (!busy_started.load()) std::this_thread::yield();
    std::thread::id ai_ran_on;
    JobCounter ai_job;
    js.spawn(ai_job, [&] { ai_ran_on = std::this_thread::get_id(); }, Priority::Ai);
    std::atomic<uint32_t> sum{0};
    js.parallel_for(64, 1, [&](uint32_t b, uint32_t e) { sum.fetch_add(e - b); });  // critical: returns without the AI job
    CHECK(sum.load() == 64);
    CHECK(!ai_job.done());
    {
        ScopedPriority ai(js, Priority::Ai);
        js.wait(ai_job);  // claims and runs its own AI job while the worker is busy
    }
    CHECK(ai_ran_on == std::this_thread::get_id());
    go = true;
    js.wait(busy);

    // Serial mode: AI jobs and AI loops run on the waiter; the destructor drains queued AI jobs.
    {
        JobSystem serial({0});
        ScopedPriority p(serial, Priority::Ai);
        std::atomic<uint32_t> n{0};
        JobCounter c2;
        serial.spawn(c2, [&] { n.fetch_add(1); });
        serial.parallel_for(32, 1, [&](uint32_t b, uint32_t e) { n.fetch_add(e - b); });
        serial.wait(c2);
        CHECK(n.load() == 33);
    }
    std::atomic<bool> drained{false};
    {
        JobCounter c3;  // outlives the system (header: counters of queued jobs must)
        JobSystem two({2});
        two.spawn(c3, [&] { drained = true; }, Priority::Ai);
    }  // never waited on: the destructor runs it
    CHECK(drained.load());
}

static void test_critical_between_ai_chunks() {
    section("sp1: critical jobs run between chunks of an AI loop");
    JobSystem js({2});
    constexpr uint32_t kChunks = 400;
    std::atomic<uint32_t> chunks{0}, chunks_at_critical{kChunks};
    std::thread ai([&] {
        ScopedPriority p(js, Priority::Ai);
        js.parallel_for(kChunks, 1, [&](uint32_t, uint32_t) {
            chunks.fetch_add(1);
            busy_us(200);  // busy, not sleep: Windows rounds short sleeps up to ~15 ms
        });
    });
    while (chunks.load() < 10) std::this_thread::yield();
    JobCounter c;
    js.spawn(c, [&] { chunks_at_critical = chunks.load(); });
    // Poll, not wait (a wait would run the job here): a helper that finishes an AI chunk must take it before its next
    // one, long before the loop ends (the loop's owner, the AI thread, does not yield: see the header).
    while (!c.done()) std::this_thread::yield();
    CHECK(chunks_at_critical.load() < kChunks / 2);
    ai.join();
    CHECK(chunks.load() == kChunks);
}

static void test_inline_small_forks() {
    section("sp1: small loops run inline, large ones fork");
    {
        JobSystem js({4});
        std::atomic<uint32_t> sum{0};
        for (int r = 0; r < 20; ++r) js.parallel_for(64, 1, [&](uint32_t b, uint32_t e) { sum.fetch_add(e - b); });
        CHECK(sum.load() == 20u * 64u);
        CHECK(js.inline_forks() >= 15);  // the first call measures; tiny work runs inline after that
    }
    {
        JobSystemConfig cfg;
        cfg.workers = 4;
        cfg.inline_small = false;
        JobSystem js(cfg);
        for (int r = 0; r < 10; ++r) js.parallel_for(64, 1, [](uint32_t, uint32_t) {});
        CHECK(js.inline_forks() == 0);
    }
    {
        JobSystem js({4});
        for (int r = 0; r < 3; ++r) {
            const auto before = js.executed_per_slot();
            js.parallel_for(32, 1, [](uint32_t, uint32_t) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
            const auto after = js.executed_per_slot();
            uint64_t helped = 0;
            for (size_t s = 1; s < after.size(); ++s) helped += after[s] - before[s];
            CHECK(helped > 0);  // ~32 ms of work: never inline
        }
        CHECK(js.inline_forks() == 0);
    }
    {
        // An inline loop still throws the lowest failing chunk and runs nothing after it (serial semantics). The site is
        // warmed without a throw first (a failed loop does not update the estimate), so the throwing calls run inline.
        JobSystem js({4});
        bool fail = false;
        std::atomic<uint32_t> highest{0};
        const auto body = [&](uint32_t b, uint32_t) {
            uint32_t h = highest.load();
            while (b > h && !highest.compare_exchange_weak(h, b)) {}
            if (fail && b == 3) throw std::runtime_error("3");
        };
        for (int r = 0; r < 3; ++r) js.parallel_for(16, 1, body);
        const uint64_t warm = js.inline_forks();
        CHECK(warm >= 1);
        fail = true;
        for (int r = 0; r < 5; ++r) {
            highest = 0;
            bool caught = false;
            try {
                js.parallel_for(16, 1, body);
            } catch (const std::runtime_error& e) {
                caught = std::string(e.what()) == "3";
            }
            CHECK(caught && highest.load() == 3);
        }
        CHECK(js.inline_forks() == warm + 5);
    }
}

static void test_pool_plan() {
    section("sp1: pool plan from a topology");
    auto make = [](uint32_t cores, uint32_t smt, uint8_t eff, uint32_t llc, uint32_t core_base, uint32_t id_base) {
        std::vector<LogicalCpu> v;
        for (uint32_t c = 0; c < cores; ++c)
            for (uint32_t t = 0; t < smt; ++t) {
                LogicalCpu l;
                l.cpu_set_id = id_base + c * smt + t;
                l.number = static_cast<uint8_t>(id_base - 256 + c * smt + t);
                l.core = core_base + c;
                l.llc = llc;
                l.efficiency = eff;
                v.push_back(l);
            }
        return v;
    };
    {
        CpuTopology t;  // 8 cores x 2 SMT, one L3 (7800X3D-like)
        t.cpus = make(8, 2, 0, 0, 0, 256);
        const PoolPlan p = plan_pool(t);
        CHECK(p.cores == 8 && p.workers == 6 && p.physical_total == 8 && p.logical_total == 16 && !p.hybrid);
        CHECK(p.cpu_set_ids.size() == 8 && p.cpu_set_ids[0] == 256 && p.cpu_set_ids[1] == 258);
    }
    {
        CpuTopology t;  // hybrid: 8 P-cores with SMT (class 1) + 16 E-cores (class 0), one L3
        t.cpus = make(8, 2, 1, 0, 0, 256);
        const auto e = make(16, 1, 0, 0, 8, 272);
        t.cpus.insert(t.cpus.end(), e.begin(), e.end());
        const PoolPlan p = plan_pool(t);
        CHECK(p.hybrid && p.cores == 8 && p.workers == 6 && p.physical_total == 24);
    }
    {
        CpuTopology t;  // dual CCD: 8 + 8 cores, the second L3 larger (V-cache)
        t.cpus = make(8, 2, 0, 0, 0, 256);
        const auto b = make(8, 2, 0, 1, 8, 272);
        t.cpus.insert(t.cpus.end(), b.begin(), b.end());
        t.l3.push_back({0, 0xFFFFull, 32ull << 20});
        t.l3.push_back({0, 0xFFFF0000ull, 96ull << 20});
        const PoolPlan p = plan_pool(t);
        CHECK(p.domains == 2 && p.cores == 8 && p.cpu_set_ids.size() == 8 && p.cpu_set_ids[0] == 272);
    }
    {
        CpuTopology t;  // hybrid laptop, 2 P-cores + 8 E-cores: 3 workers (P-cores alone would leave 1)
        t.cpus = make(2, 2, 1, 0, 0, 256);
        const auto e = make(8, 1, 0, 0, 2, 260);
        t.cpus.insert(t.cpus.end(), e.begin(), e.end());
        const PoolPlan p = plan_pool(t);
        CHECK(p.hybrid && p.cores == 2 && p.workers == 3);
    }
    {
        CpuTopology t;  // two cores: still one worker
        t.cpus = make(2, 1, 0, 0, 0, 256);
        CHECK(plan_pool(t).workers == 1);
        CHECK(plan_pool(CpuTopology{}).cores == 0);
    }
    CHECK(JobSystem::default_worker_count() >= 1);
    std::printf("   machine: %s\n", machine_pool().describe().c_str());
}

int main() {
    test_config();
    test_stress();
    test_nested();
    test_exceptions();
    test_serial_mode();
    test_reduce_determinism();
    test_map_commit();
    test_bool_results();
    test_tick_rate();
    test_tick_cadence();
    test_deferred();
    test_shutdown();
    test_background_priority();
    test_background_isolation();
    test_abort_skips_later_chunks();
    test_for_waits_on_chunks();
    test_background_cap();
    test_scratch();
    test_priority_order();
    test_critical_between_ai_chunks();
    test_inline_small_forks();
    test_pool_plan();
    std::printf(g_fail ? "alamo_jobs_tests: %d failure(s)\n" : "alamo_jobs_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
