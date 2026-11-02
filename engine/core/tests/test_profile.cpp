// OPT1 profiler: zones, laps, ring wrap, threads, percentile report.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "alamo/core/profile.hpp"

namespace {
int g_fail = 0;
#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

using namespace alamo::profile;

void busy_ms(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
    }
}

void test_registry() {
    const ZoneId a = register_zone("test.a");
    const ZoneId b = register_zone("test.b");
    CHECK(a != kNoZone && b != kNoZone && a != b);
    CHECK(register_zone("test.a") == a);
    CHECK(std::string(zone_name(b)) == "test.b");
}

void test_disabled_is_inert() {
    stop();
    CHECK(!enabled());
    begin_tick(1);
    {
        ALAMO_PROFILE_SCOPE("test.disabled");
        busy_ms(1);
    }
    end_tick();
    start(8);
    begin_tick(2);
    end_tick();
    const auto snap = snapshot();
    CHECK(snap.size() == 1);
    CHECK(snap[0].zone_ns[register_zone("test.disabled")] == 0);  // the disabled scope left nothing behind
    stop();
}

void test_scope_and_lap() {
    start(8);
    const ZoneId z = register_zone("test.scope");
    begin_tick(10);
    {
        Scope s(z);
        busy_ms(3);
    }
    busy_ms(2);
    lap("test.lap");
    end_tick();
    const auto snap = snapshot();
    CHECK(snap.size() == 1);
    CHECK(snap[0].frame == 10);
    CHECK(snap[0].zone_ns[z] >= 2'500'000u);
    CHECK(snap[0].zone_ns[register_zone("test.lap")] >= 4'500'000u);  // lap covers the scope and the busy wait
    CHECK(snap[0].total_ns >= snap[0].zone_ns[register_zone("test.lap")]);
    stop();
}

void test_ring_wrap() {
    start(4);
    for (uint64_t f = 0; f < 10; ++f) {
        begin_tick(f);
        end_tick();
    }
    CHECK(ticks_recorded() == 10);
    const auto snap = snapshot();
    CHECK(snap.size() == 4);
    CHECK(snap.front().frame == 6 && snap.back().frame == 9);
    const auto newest = snapshot(2);  // the newest two, oldest first
    CHECK(newest.size() == 2 && newest.front().frame == 8 && newest.back().frame == 9);
    CHECK(snapshot(0).empty());
    stop();
}

void test_threads_sum_exactly() {
    start(4);
    const ZoneId z = register_zone("test.threads");
    begin_tick(0);
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
        th.emplace_back([z] {
            for (int i = 0; i < 1000; ++i) add_zone_ns(z, 1000);
            add_busy_ns(3, 500);
        });
    for (auto& t : th) t.join();
    end_tick();
    const auto snap = snapshot();
    CHECK(snap.size() == 1);
    CHECK(snap[0].zone_ns[z] == 8u * 1000u * 1000u);
    CHECK(snap[0].busy_ns[3] == 8u * 500u);
    stop();
}

void test_report() {
    const ZoneId z = register_zone("test.report");
    std::vector<TickRecord> ticks(100);
    for (uint32_t i = 0; i < 100; ++i) {
        ticks[i].frame = i;
        ticks[i].total_ns = (i + 1) * 1'000'000u;  // 1..100 ms
        ticks[i].zone_ns[z] = i == 99 ? 90'000'000u : 0u;
        ticks[i].busy_ns[1] = 500'000u;
    }
    const Report r = build_report(ticks, 1000.0 / 30.0, 3);
    CHECK(r.ticks == 100);
    CHECK(r.p50_ms == 51.0);
    CHECK(r.p99_ms == 100.0);
    CHECK(r.max_ms == 100.0);
    CHECK(r.over_budget == 67);  // 34..100 ms exceed 33.33 ms
    CHECK(r.worst.size() == 3 && r.worst[0].frame == 99 && r.worst[0].total_ms == 100.0);
    CHECK(!r.worst[0].top.empty() && r.worst[0].top[0].first == "test.report");
    bool found = false;
    for (const ZoneStat& zs : r.zones)
        if (zs.name == "test.report") {
            found = true;
            CHECK(zs.active_ticks == 1 && zs.max_ms == 90.0 && zs.p50_ms == 0.0);
        }
    CHECK(found);
    CHECK(r.busy_share.size() == 2 && r.busy_share[0] == 0.0 && r.busy_share[1] > 0.0);
    CHECK(!format_report(r).empty());
    CHECK(build_report({}).ticks == 0);
}
}  // namespace

int main() {
    test_registry();
    test_disabled_is_inert();
    test_scope_and_lap();
    test_ring_wrap();
    test_threads_sum_exactly();
    test_report();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("profile tests OK\n");
    return 0;
}
