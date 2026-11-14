// alamo_tactical_services_tests (no framework; non-zero exit on failure). SP5: the services phase graph
// (service_graph.hpp), deferred-write buffers (deferred.hpp) and counter-based streams (stream_rng.hpp).
//   * phases: conflicting services keep declaration order, disjoint ones share a phase, order() edges and barriers split;
//   * Serial, Parallel (1, 4, 8 threads) and Reverse runs of a declared-correctly workload give identical state;
//   * the Checker reports an undeclared write (and records observed writes); Reverse exposes an undeclared read;
//   * DeferredBuffer commits in key order whatever the lane split and the thread count;
//   * stream seeds are fixed values (part of the outcome) and differ per tick / stream / entity.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "alamo/tactical/services/deferred.hpp"
#include "alamo/tactical/services/service_graph.hpp"
#include "alamo/tactical/services/stream_rng.hpp"
#include "alamo/test/support.hpp"
#include "alamo/test/thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::tactical::services;
using ::alamo::test::g_fail;

namespace {

enum : uint32_t { kA, kB, kC, kD, kRng };

void test_phases() {
    test::section("phases");
    ServiceGraph g;
    const auto noop = [] {};
    const ServiceId a = g.add("a", component(kA), component(kB), noop);   // writes B
    const ServiceId b = g.add("b", component(kC), component(kD), noop);   // disjoint from a
    const ServiceId c = g.add("c", component(kB), component(kC), noop);   // reads B (a), writes C (b reads): after both
    const ServiceId d = g.add("d", component(kA), 0, noop);               // reads A only: phase 0
    const ServiceId e = g.add("e", 0, component(kA), noop);               // writes A: after a (reads A) and d
    const ServiceId f = g.add("f", 0, component(kRng), noop);
    const ServiceId h = g.add("h", 0, component(kRng), noop);             // same stream: after f
    const ServiceId bar = g.barrier("lap", noop);
    const ServiceId i = g.add("i", 0, component(kD), noop);
    const ServiceId j = g.add("j", 0, 0, noop);
    g.order(i, j);  // explicit retail order
    g.build();
    CHECK(g.info(a).phase == 0);
    CHECK(g.info(b).phase == 0);
    CHECK(g.info(c).phase == 1);
    CHECK(g.info(d).phase == 0);
    CHECK(g.info(e).phase == 1);
    CHECK(g.info(f).phase == 0);
    CHECK(g.info(h).phase == 1);
    CHECK(g.info(bar).phase == 2);
    CHECK(g.info(i).phase == 3);
    CHECK(g.info(j).phase == 4);
    CHECK(g.phase_count() == 5);
    // within a phase: pairwise disjoint, ascending declaration order
    for (uint32_t p = 0; p < g.phase_count(); ++p) {
        const auto ph = g.phase(p);
        for (size_t x = 0; x < ph.size(); ++x)
            for (size_t y = x + 1; y < ph.size(); ++y) {
                CHECK(ph[x] < ph[y]);
                CHECK(!ServiceGraph::conflict(g.info(ph[x]), g.info(ph[y])));
            }
    }
    CHECK(!g.describe().empty());
}

// A workload over integer "components": each service mixes its read components into its written ones.
struct World {
    uint64_t v[5] = {1, 2, 3, 4, 5};
};

void build_workload(ServiceGraph& g, World& w) {
    g.clear();
    struct S {
        uint32_t r1, r2, wr;
    };
    const S list[] = {{kA, kA, kB}, {kC, kC, kD}, {kB, kD, kA}, {kA, kA, kC}, {kRng, kRng, kRng}, {kB, kB, kD}, {kD, kC, kRng}};
    for (size_t n = 0; n < std::size(list); ++n) {
        const S s = list[n];
        g.add("s" + std::to_string(n), component(s.r1) | component(s.r2), component(s.wr), [&w, s, n] {
            w.v[s.wr] = w.v[s.wr] * 6364136223846793005ull + w.v[s.r1] * 31 + w.v[s.r2] * 7 + n;
        });
    }
}

void test_modes() {
    test::section("modes");
    World ref;
    ServiceGraph g;
    build_workload(g, ref);
    for (int t = 0; t < 50; ++t) g.run(serial_for, RunMode::Serial);
    for (unsigned threads : {1u, 4u, 8u}) {
        test::ThreadPoolFor pool(threads);
        const ParallelFor pf = [&pool](uint32_t n, uint32_t gr, const RangeFn& fn) { pool.run(n, gr, fn); };
        World w;
        ServiceGraph g2;
        build_workload(g2, w);
        for (int t = 0; t < 50; ++t) g2.run(pf, RunMode::Parallel);
        for (uint32_t c = 0; c < 5; ++c) CHECK(w.v[c] == ref.v[c]);
    }
    World rv;
    ServiceGraph g3;
    build_workload(g3, rv);
    for (int t = 0; t < 50; ++t) g3.run(serial_for, RunMode::Reverse);
    for (uint32_t c = 0; c < 5; ++c) CHECK(rv.v[c] == ref.v[c]);
}

void test_checks() {
    test::section("checks");
    World w;
    ServiceGraph g;
    g.add("honest", component(kA), component(kB), [&w] { w.v[kB] += w.v[kA]; });
    g.add("liar", component(kA), component(kC), [&w] { w.v[kD] += 1; });  // writes D, declares C
    Checker ck;
    ck.checkable = component(kA) | component(kB) | component(kC) | component(kD);
    ck.digest = [&w](uint32_t c) { return w.v[c]; };
    std::vector<std::string> seen;
    ck.on_violation = [&seen](const std::string& s, uint32_t c) { seen.push_back(s + ":" + std::to_string(c)); };
    g.run(serial_for, RunMode::Parallel, &ck);  // a checker forces serial
    CHECK(ck.violations == 1);
    CHECK(seen.size() == 1 && seen[0] == "liar:3");
    CHECK(g.info(0).observed_writes == component(kB));
    CHECK(g.info(1).observed_writes == component(kD));

    // The reads half: "r" reads C without declaring it and shares phase 0 with C's writer; Reverse order changes it.
    World a, b;
    const auto make = [](ServiceGraph& gr, World& x) {
        gr.add("w", 0, component(kC), [&x] { x.v[kC] += 1; });
        gr.add("r", component(kA), component(kB), [&x] { x.v[kB] += x.v[kC]; });
    };
    ServiceGraph ga, gb;
    make(ga, a);
    make(gb, b);
    ga.build();
    CHECK(ga.phase_count() == 1);
    for (int t = 0; t < 10; ++t) {
        ga.run(serial_for, RunMode::Serial);
        gb.run(serial_for, RunMode::Reverse);
    }
    bool differs = false;
    for (uint32_t c = 0; c < 5; ++c) differs = differs || a.v[c] != b.v[c];
    CHECK(differs);
}

void test_deferred() {
    test::section("deferred");
    // 200 sources, each emitting (i % 3) + 1 records; the committed sequence must be the key order for any split.
    std::vector<std::pair<Key, uint32_t>> ref;
    for (uint32_t i = 0; i < 200; ++i)
        for (uint32_t s = 0; s <= i % 3; ++s) ref.push_back({Key{7, i, s}, i * 10 + s});
    for (unsigned threads : {1u, 4u, 8u})
        for (uint32_t grain : {1u, 7u, 64u}) {
            test::ThreadPoolFor pool(threads);
            DeferredBuffer<uint32_t> buf;
            buf.begin(200);
            pool.run(200, grain, [&buf](uint32_t b, uint32_t e) {
                for (uint32_t i = e; i-- > b;) {  // reversed within the range on purpose
                    auto lane = buf.lane(i);
                    for (uint32_t s = i % 3 + 1; s-- > 0;) lane.push(Key{7, i, s}, i * 10 + s);
                }
            });
            CHECK(buf.pending() == ref.size());
            std::vector<std::pair<Key, uint32_t>> got;
            buf.commit([&got](const Key& k, uint32_t& v) { got.push_back({k, v}); });
            CHECK(got == ref);
            CHECK(buf.pending() == 0);
        }
}

void test_streams() {
    test::section("streams");
    // Fixed values: part of the outcome once a flag is on by default; a change here moves goldens.
    CHECK(stream_seed(7, 300, StreamId::Fog, 42) == stream_seed(7, 300, StreamId::Fog, 42));
    const uint32_t s0 = stream_seed(7, 300, StreamId::Fog, 42);
    CHECK(s0 != stream_seed(8, 300, StreamId::Fog, 42));
    CHECK(s0 != stream_seed(7, 301, StreamId::Fog, 42));
    CHECK(s0 != stream_seed(7, 300, StreamId::Hangar, 42));
    CHECK(s0 != stream_seed(7, 300, StreamId::Fog, 43));
    std::printf("stream_seed(7, 300, Fog, 42) = 0x%08x\n", static_cast<unsigned>(s0));
    CHECK(stream_mix(0) == 0xE220A8397B1DCDAFull);  // splitmix64 of 0 (reference value)
    // Draw ranges unchanged: range_int over many entity streams covers [lo, hi] and stays in it.
    int lo_seen = 100, hi_seen = -1;
    for (uint32_t e = 0; e < 2000; ++e) {
        SimRng r = stream_rng(1, 5, StreamId::Combat, e);
        const int32_t v = r.range_int(0, 9);
        lo_seen = std::min(lo_seen, v);
        hi_seen = std::max(hi_seen, v);
    }
    CHECK(lo_seen == 0 && hi_seen == 9);
}

}  // namespace

int main() {
    test_phases();
    test_modes();
    test_checks();
    test_deferred();
    test_streams();
    return test::report("alamo_tactical_services_tests");
}
