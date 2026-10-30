// alamo_tactical_bench [workers] - S6 spatial tree micro-benchmark: ns/query per query kind at
// 1K / 16K / 64K entries (vs brute force), update/rebuild cost, and serial vs job-system batches.
// Scene: a 20 km x 2 km x 20 km space with 1..40 unit half-size boxes, 8 categories.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "alamo/jobs/job_system.hpp"
#include "alamo/tactical/spatial_tree.hpp"

using namespace alamo;
using namespace alamo::tactical;

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1 << 24); }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

size_t g_sink = 0;
using Clock = std::chrono::steady_clock;
double ns_since(Clock::time_point t0) { return std::chrono::duration<double, std::nano>(Clock::now() - t0).count(); }

Vec3 rpoint(Rng& r) { return {r.range(-10000, 10000), r.range(-1000, 1000), r.range(-10000, 10000)}; }

Frustum camera(Rng& r) {
    const Vec3 eye = rpoint(r);
    const Vec3 fwd{r.range(-1, 1), -0.5f, r.range(-1, 1)};
    const Vec3 right = cross(fwd, Vec3{0, 1, 0});
    const Vec3 up = cross(right, fwd);
    const float n = 5.0f, f = 3000.0f, s = 0.4f;
    Vec3 nq[4], fq[4];
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        nq[i] = eye + fwd * n + right * (sx[i] * n * s) + up * (sy[i] * n * s);
        fq[i] = eye + fwd * f + right * (sx[i] * f * s) + up * (sy[i] * f * s);
    }
    return frustum_from_corners(nq, fq);
}

}  // namespace

int main(int argc, char** argv) {
    const int32_t workers = argc > 1 ? std::atoi(argv[1]) : -1;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    const ParallelFor pf = jobs::make_parallel_for(js);
    std::printf("alamo_tactical_bench: %u threads (workers + caller)\n", js.thread_count());
    std::printf("%7s | %9s %9s %9s %9s %9s %9s | %7s | %9s %9s | %9s %9s %9s\n", "entries", "sphere", "aabb", "frustum",
                "ray", "ray1st", "visible", "avg_hits", "brute_sph", "update", "rebuild_s", "rebuild_p", "batch_s/p");
    for (uint32_t n : {1024u, 16384u, 65536u}) {
        Rng r(n);
        SpatialTreeConfig cfg;
        cfg.fat_margin = 2.0f;
        SpatialTree t(cfg);
        std::vector<Aabb> boxes(n);
        for (uint32_t i = 0; i < n; ++i) {
            const Vec3 c = rpoint(r);
            const float h = r.range(1, 40);
            boxes[i] = {c - Vec3{h, h, h}, c + Vec3{h, h, h}};
            t.insert(i, boxes[i], 1u << (i & 7));
        }
        t.rebuild();
        const size_t Q = 4000;
        std::vector<Sphere> sph(Q);
        std::vector<Aabb> qb(Q);
        std::vector<Frustum> fr(Q);
        std::vector<Ray> rays(Q);
        for (size_t i = 0; i < Q; ++i) {
            const Vec3 c = rpoint(r);
            sph[i] = {c, 800.0f};
            qb[i] = {c - Vec3{600, 600, 600}, c + Vec3{600, 600, 600}};
            fr[i] = camera(r);
            rays[i] = {c, {r.range(-1, 1), r.range(-0.1f, 0.1f), r.range(-1, 1)}, 3000.0f};
        }
        QueryContext ctx;
        ctx.reserve(n);
        size_t hits = 0, sink = 0;
        auto time_q = [&](auto&& fn) {
            for (size_t i = 0; i < 200; ++i) fn(i);  // warm-up
            const auto t0 = Clock::now();
            for (size_t i = 0; i < Q; ++i) fn(i);
            return ns_since(t0) / static_cast<double>(Q);
        };
        const double ns_sph = time_q([&](size_t i) { hits += t.query_sphere(sph[i], kAllCategories, ctx).size(); });
        const double ns_box = time_q([&](size_t i) { sink += t.query_aabb(qb[i], 0x0F, ctx).size(); });
        const double ns_fr = time_q([&](size_t i) { sink += t.query_frustum(fr[i], kAllCategories, ctx).size(); });
        const double ns_ray = time_q([&](size_t i) { sink += t.query_ray(rays[i], kAllCategories, ctx).size(); });
        const double ns_r1 = time_q([&](size_t i) {
            RayHit h;
            sink += t.raycast_first(rays[i], kAllCategories, ctx, h) ? 1 : 0;
        });
        const double ns_vis =
            time_q([&](size_t i) { sink += t.query_visible(sph[i], 0x0F, 0xF0, kNoEntity, ctx).size(); });
        // brute force sphere (fewer queries at 64K)
        const size_t QB = n > 20000 ? 200 : 1000;
        auto t0 = Clock::now();
        for (size_t i = 0; i < QB; ++i)
            for (uint32_t k = 0; k < n; ++k) sink += overlaps(sph[i], boxes[k]) ? 1 : 0;
        const double ns_brute = ns_since(t0) / static_cast<double>(QB);
        // updates: every entry moves 5 units (fat margin 2 -> most leave their fat box)
        t0 = Clock::now();
        for (uint32_t i = 0; i < n; ++i) {
            boxes[i] = {boxes[i].min + Vec3{5, 0, 0}, boxes[i].max + Vec3{5, 0, 0}};
            t.update(i, boxes[i]);
        }
        const double ns_upd = ns_since(t0) / n;
        // rebuild serial vs parallel (parallel engages only at >= 16K entries)
        auto time_rebuild = [&](const ParallelFor& p) {
            t.rebuild(p);
            const auto r0 = Clock::now();
            for (int k = 0; k < 5; ++k) t.rebuild(p);
            return ns_since(r0) / 5 / 1e6;
        };
        const double ms_rs = time_rebuild(serial_for);
        const double ms_rp = time_rebuild(pf);
        // batches of 4096 sphere queries
        std::vector<BatchQuery> bq(4096);
        for (size_t i = 0; i < bq.size(); ++i) {
            bq[i].kind = QueryKind::Sphere;
            bq[i].sphere = sph[i % Q];
        }
        BatchResults br;
        t.query_batch(bq, br, serial_for);
        t0 = Clock::now();
        t.query_batch(bq, br, serial_for);
        const double ns_bs = ns_since(t0) / static_cast<double>(bq.size());
        t.query_batch(bq, br, pf);
        t0 = Clock::now();
        t.query_batch(bq, br, pf);
        const double ns_bp = ns_since(t0) / static_cast<double>(bq.size());
        std::printf("%7u | %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f | %7.1f | %9.0f %9.0f | %8.2fms %8.2fms %5.0f/%-5.0f\n", n,
                    ns_sph, ns_box, ns_fr, ns_ray, ns_r1, ns_vis, static_cast<double>(hits) / static_cast<double>(Q + 200), ns_brute,
                    ns_upd, ms_rs, ms_rp, ns_bs, ns_bp);
        g_sink += sink;
    }
    std::printf("(ns/query; update ns/entry; batch ns/query serial/job-system - parallel engages at >= %u entries)\n",
                SpatialTreeConfig{}.parallel_threshold);
    std::printf("checksum %zu\n", g_sink);
    return 0;
}
