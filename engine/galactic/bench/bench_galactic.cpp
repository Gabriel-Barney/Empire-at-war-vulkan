// alamo_galactic_bench: AI all-pairs (FW reference serial / row-parallel vs eligible fast path) and fleet A* throughput
// on synthetic galaxies. Usage: alamo_galactic_bench [threads]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/galactic/planet_graph.hpp"
#include "alamo/test/thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::galactic;

namespace {

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1 << 24); }
};

// ~3 trade routes per planet plus free hops (max 100) at a density of ~3 neighbours in range.
GalaxyMap make_galaxy(uint32_t n, uint64_t seed) {
    Rng r{seed};
    GalaxyMap m;
    m.max_fleet_move_distance = 100.0f;
    const float extent = std::sqrt(float(n) * 3.14159265f * 10000.0f / 3.0f);
    for (uint32_t i = 0; i < n; ++i) {
        const auto p = m.planets.add("P" + std::to_string(i));
        m.planets.position[p] = {r.unit() * extent, r.unit() * extent, 0.0f};
        m.planets.flags[p] = kPlanetHasPlanetBehavior;
    }
    static const float speeds[] = {1.0f, 1.0f, 1.5f, 2.0f};
    for (uint32_t k = 0; k < n * 3 / 2; ++k) {
        const uint32_t a = r.below(n), b = r.below(n);
        if (a == b) continue;
        const auto ri = m.routes.add("R" + std::to_string(k));
        m.routes.point_a[ri] = a;
        m.routes.point_b[ri] = b;
        m.routes.hs_speed_factor[ri] = speeds[r.below(4)];
    }
    return m;
}

template <class F>
double time_ms(F&& f, int min_reps, double min_total_ms) {
    double best = 1e300, total = 0;
    for (int rep = 0; rep < min_reps || total < min_total_ms; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        total += ms;
        if (rep > 1000) break;
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = argc > 1 ? static_cast<unsigned>(std::max(1, std::atoi(argv[1]))) : hw;
    const ParallelFor par = ::alamo::test::make_thread_parallel_for(threads);
    std::printf("hardware_concurrency = %u, bench threads = %u\n", hw, threads);
    std::printf("times are best-of-N wall clock, ms. 'fast' columns run only when the fast path is eligible\n");
    std::printf("(no uint32 wrap possible); 'auto' = compute_all_pairs (fast iff eligible && N >= %u).\n\n",
                kFastAllPairsMinPlanets);
    std::printf("%6s %6s %7s %5s %9s %9s %9s %9s %7s %7s %6s %6s %10s %9s\n", "N", "costs", "edges", "elig",
                "FW 1T", "FW MT", "fast 1T", "fast MT", "x(1T)", "x(MT)", "auto", "equal", "A* 10k", "us/query");
    for (uint32_t n : {50u, 100u, 128u, 150u, 200u, 400u}) {
        for (int neg = 0; neg < 2; ++neg) {
            if (neg && n != 200) continue;
            const GalaxyMap gm = make_galaxy(n, 0xC0FFEE + n);
            const PlanetGraph g = PlanetGraph::build(gm);
            Rng r{n};
            // neg == 0: small perception totals (eligible). neg == 1: some negative totals, which
            // wrap to node cost 42949672 (Q7) and make uint32 sums wrap => reference only.
            std::vector<double> value(n);
            for (auto& v : value) v = neg && r.below(4) == 0 ? -50.0 : double(r.below(1000));
            AllPairsInput in;
            in.node_value = value;
            detail::RawOwned raw;
            detail::build_raw(g, in, raw);
            const bool elig = detail::fast_path_eligible(raw.view());
            AllPairsMatrices ref, refp, fast1, fastp, autom;
            const double t_ref = time_ms([&] { reference_all_pairs(g, in, ref); }, 3, 300);
            const double t_refp = time_ms([&] { reference_all_pairs(g, in, refp, par); }, 3, 300);
            double t_f1 = 0, t_fp = 0;
            bool equal = ref == refp;
            if (elig) {
                t_f1 = time_ms([&] { fast_all_pairs(g, in, fast1, serial_for); }, 5, 300);
                t_fp = time_ms([&] { fast_all_pairs(g, in, fastp, par); }, 5, 300);
                equal = equal && ref == fast1 && ref == fastp;
            }
            const AllPairsMethod am = compute_all_pairs(g, in, autom, par);
            equal = equal && ref == autom;

            std::vector<PathRequest> reqs(10000);
            for (auto& q : reqs) {
                q.source = r.below(n);
                q.destination = r.below(n);
            }
            PathScratch scratch;
            PlayerView view;
            std::vector<PlanetIndex> out;
            size_t found = 0;
            const double t_astar = time_ms(
                [&] {
                    found = 0;
                    for (const auto& q : reqs) found += find_fleet_path(g, view, q, scratch, out);
                },
                3, 200);
            char f1[16] = "-", fp[16] = "-", x1[16] = "-", xp[16] = "-";
            if (elig) {
                std::snprintf(f1, sizeof f1, "%.3f", t_f1);
                std::snprintf(fp, sizeof fp, "%.3f", t_fp);
                std::snprintf(x1, sizeof x1, "%.1f", t_ref / t_f1);
                std::snprintf(xp, sizeof xp, "%.1f", t_ref / t_fp);
            }
            std::printf("%6u %6s %7u %5s %9.3f %9.3f %9s %9s %7s %7s %6s %6s %10.3f %9.3f\n", n, neg ? "neg" : "small",
                        g.edge_slots() / 2, elig ? "yes" : "no", t_ref, t_refp, f1, fp, x1, xp,
                        am == AllPairsMethod::Fast ? "fast" : "ref", equal ? "yes" : "NO", t_astar,
                        t_astar * 1000.0 / 10000.0);
        }
    }
    return 0;
}
