// alamo_jobs_bench [max_workers]
// Overhead per job / per parallel_for call, and speedup on a CPU-bound kernel (independent
// Dijkstra runs, the shape of the AI all-pairs recompute) for 0..max_workers workers.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "alamo/jobs/jobs.hpp"

using namespace alamo::jobs;
using Clock = std::chrono::steady_clock;

static double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Sparse random graph (deterministic LCG), ~4 edges per node, like a big galactic map scaled up.
struct Graph {
    uint32_t n = 0;
    std::vector<uint32_t> offs, to;
    std::vector<float> w;
};

static Graph make_graph(uint32_t n, uint32_t degree) {
    Graph g;
    g.n = n;
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(n);
    uint32_t s = 12345;
    auto rnd = [&] {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    };
    for (uint32_t i = 0; i < n; ++i) {
        adj[i].emplace_back((i + 1) % n, 1.0f);  // ring keeps it connected
        for (uint32_t k = 1; k < degree; ++k) {
            const uint32_t j = rnd() % n;
            const float c = 1.0f + static_cast<float>(rnd() % 1000) / 100.0f;
            adj[i].emplace_back(j, c);
            adj[j].emplace_back(i, c);
        }
    }
    g.offs.push_back(0);
    for (auto& a : adj) {
        for (auto [j, c] : a) {
            g.to.push_back(j);
            g.w.push_back(c);
        }
        g.offs.push_back(static_cast<uint32_t>(g.to.size()));
    }
    return g;
}

// Single-source Dijkstra; returns a checksum of the distance row.
static double dijkstra(const Graph& g, uint32_t src, std::vector<float>& dist) {
    dist.assign(g.n, 1e30f);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    dist[src] = 0.0f;
    pq.emplace(0.0f, src);
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) continue;
        for (uint32_t e = g.offs[u]; e < g.offs[u + 1]; ++e) {
            const float nd = d + g.w[e];
            if (nd < dist[g.to[e]]) {
                dist[g.to[e]] = nd;
                pq.emplace(nd, g.to[e]);
            }
        }
    }
    double sum = 0.0;
    for (float x : dist) sum += x;
    return sum;
}

int main(int argc, char** argv) {
    const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    uint32_t max_workers = std::max<uint32_t>(3u, hc);  // include oversubscription on small boxes
    if (argc > 1) max_workers = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
    std::printf("alamo_jobs_bench: hardware_concurrency=%u default_workers=%u\n\n", hc,
                JobSystem::default_worker_count());

    // --- overhead -----------------------------------------------------------------------------
    std::printf("%-8s %14s %16s %18s %16s\n", "workers", "submit ns/job", "pfor ns/chunk", "pfor call us(64)",
                "spawn ns/job");
    for (uint32_t w = 0; w <= max_workers; ++w) {
        JobSystemConfig cfg;
        cfg.workers = static_cast<int32_t>(w);
        cfg.inline_small = false;  // sp1: measure the fork path, not the inline shortcut
        JobSystem js(cfg);
        // raw submit + wait of empty jobs, batches of 4096
        const uint32_t jobs = 1u << 20;
        JobCounter c;
        auto t0 = Clock::now();
        for (uint32_t done = 0; done < jobs; done += 4096) {
            for (uint32_t i = 0; i < 4096; ++i) js.submit(c, [](void*, uint32_t, uint32_t) {}, nullptr, i);
            js.wait(c);
        }
        const double submit_ns = seconds_since(t0) * 1e9 / jobs;

        // parallel_for grain 1 with an empty body: per-chunk scheduling cost
        const uint32_t n = 1u << 21;
        t0 = Clock::now();
        js.parallel_for(n, 1, [](uint32_t, uint32_t) {});
        const double pfor_ns = seconds_since(t0) * 1e9 / n;

        // latency of a small parallel_for call (fan-out + join)
        const uint32_t calls = 20000;
        std::vector<uint32_t> out(64);
        t0 = Clock::now();
        for (uint32_t r = 0; r < calls; ++r)
            js.parallel_for(64, 1, [&](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) out[i] = i ^ r;
            });
        const double call_us = seconds_since(t0) * 1e6 / calls;

        // spawn (heap closure)
        t0 = Clock::now();
        for (uint32_t done = 0; done < jobs / 4; done += 4096) {
            for (uint32_t i = 0; i < 4096; ++i) js.spawn(c, [] {});
            js.wait(c);
        }
        const double spawn_ns = seconds_since(t0) * 1e9 / (jobs / 4);
        std::printf("%-8u %14.1f %16.1f %18.2f %16.1f\n", w, submit_ns, pfor_ns, call_us, spawn_ns);
    }

    // --- speedup: 200 independent Dijkstra runs --------------------------------------------------
    const Graph g = make_graph(20000, 4);
    const uint32_t sources = 200;
    std::printf("\nkernel: %u single-source Dijkstra on %u nodes / %zu arcs\n", sources, g.n, g.to.size());
    std::printf("%-8s %10s %10s %10s %12s\n", "workers", "ms", "speedup", "eff/thr", "checksum");
    double base_ms = 0.0, ref_sum = 0.0;
    for (uint32_t w = 0; w <= max_workers; ++w) {
        JobSystemConfig cfg;
        cfg.workers = static_cast<int32_t>(w);
        cfg.inline_small = false;  // sp1: measure the fork path, not the inline shortcut
        JobSystem js(cfg);
        std::vector<double> per(sources);
        double best = 1e30;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = Clock::now();
            js.parallel_for(sources, 1, [&](uint32_t b, uint32_t e) {
                thread_local std::vector<float> dist;
                for (uint32_t s = b; s < e; ++s) per[s] = dijkstra(g, (s * 97u) % g.n, dist);
            });
            best = std::min(best, seconds_since(t0) * 1e3);
        }
        double sum = 0.0;
        for (double v : per) sum += v;  // ordered combine -> identical for every w
        if (w == 0) {
            base_ms = best;
            ref_sum = sum;
        }
        const double sp = base_ms / best;
        std::printf("%-8u %10.1f %10.2f %10.2f %12.6g%s\n", w, best, sp, sp / (w ? w + 1 : 1), sum,
                    sum == ref_sum ? "" : "  MISMATCH");
    }

    // --- deterministic reduce cost ---------------------------------------------------------------
    std::vector<float> v(1u << 24);
    for (uint32_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i % 1000) * 0.001f;
    std::printf("\nparallel_reduce float sum of %zu values (chunk 4096)\n%-8s %10s %14s\n", v.size(), "workers",
                "ms", "result");
    for (uint32_t w = 0; w <= max_workers; ++w) {
        JobSystemConfig cfg;
        cfg.workers = static_cast<int32_t>(w);
        cfg.inline_small = false;  // sp1: measure the fork path, not the inline shortcut
        JobSystem js(cfg);
        double best = 1e30;
        float r = 0.0f;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = Clock::now();
            r = parallel_reduce(
                js, static_cast<uint32_t>(v.size()), 4096, 0.0f,
                [&](uint32_t b, uint32_t e) {
                    float s = 0.0f;
                    for (uint32_t i = b; i < e; ++i) s += v[i];
                    return s;
                },
                [](float a, float b) { return a + b; });
            best = std::min(best, seconds_since(t0) * 1e3);
        }
        std::printf("%-8u %10.2f %14a\n", w, best, static_cast<double>(r));
    }
    return 0;
}
