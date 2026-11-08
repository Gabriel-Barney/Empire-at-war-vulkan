// alamo_tactical tests (no framework; non-zero exit on failure).
// S6 spatial tree: every query is checked against a brute-force reference over a model of the
// entry set on fixed-seed scenes (random op streams and a moving "battle" scene); rebuild is
// shown history- and worker-count-independent; queries are shown allocation-free.
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <thread>
#include <vector>

#include "alamo/tactical/spatial_tree.hpp"
#include "alamo/tactical/tree_set.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

// ---- allocation counter (replaces global operator new for this executable) ----
static std::atomic<uint64_t> g_allocs{0};
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace alamo;
using namespace alamo::tactical;

using ::alamo::test::g_fail;

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
    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1 << 24); }
    float range(float a, float b) { return a + (b - a) * unit(); }
    bool chance(float p) { return unit() < p; }
};

struct Item {
    Aabb box;
    uint32_t mask;
};
using Model = std::map<EntityHandle, Item>;  // ordered: brute results come out ascending

// ---------------- brute-force reference ----------------
std::vector<EntityHandle> bf_aabb(const Model& m, const Aabb& q, QueryFilter mask) {
    std::vector<EntityHandle> r;
    for (const auto& [h, it] : m)
        if (mask.matches(it.mask) && overlaps(q, it.box)) r.push_back(h);
    return r;
}
std::vector<EntityHandle> bf_sphere(const Model& m, const Sphere& q, QueryFilter mask) {
    std::vector<EntityHandle> r;
    for (const auto& [h, it] : m)
        if (mask.matches(it.mask) && overlaps(q, it.box)) r.push_back(h);
    return r;
}
std::vector<EntityHandle> bf_frustum(const Model& m, const Frustum& q, QueryFilter mask) {
    std::vector<EntityHandle> r;
    for (const auto& [h, it] : m)
        if (mask.matches(it.mask) && overlaps(q, it.box)) r.push_back(h);
    return r;
}
std::vector<EntityHandle> bf_circle(const Model& m, const CircleXY& q, QueryFilter mask) {
    std::vector<EntityHandle> r;
    for (const auto& [h, it] : m)
        if (mask.matches(it.mask) && overlaps(q, it.box)) r.push_back(h);
    return r;
}
std::vector<RayHit> bf_ray(const Model& m, const Ray& q, QueryFilter mask) {
    std::vector<RayHit> r;
    const PreparedRay pr(q);
    for (const auto& [h, it] : m) {
        float t = 0.0f;
        if (mask.matches(it.mask) && ray_aabb(pr, it.box, t)) r.push_back({h, t});
    }
    std::stable_sort(r.begin(), r.end(), [](const RayHit& a, const RayHit& b) { return a.t < b.t; });
    return r;  // stable over ascending handles == (t, handle) order
}
bool bf_blocked(const Model& m, Vec3 a, Vec3 b, QueryFilter mask, EntityHandle i0, EntityHandle i1) {
    const PreparedRay pr(Ray{a, b - a, 1.0f});
    for (const auto& [h, it] : m) {
        float t = 0.0f;
        if (h != i0 && h != i1 && mask.matches(it.mask) && ray_aabb(pr, it.box, t)) return true;
    }
    return false;
}
std::vector<EntityHandle> bf_visible(const Model& m, const Sphere& v, QueryFilter tmask, QueryFilter bmask, EntityHandle obs) {
    std::vector<EntityHandle> r;
    for (EntityHandle h : bf_sphere(m, v, tmask))
        if (h != obs && !bf_blocked(m, v.center, center(m.at(h).box), bmask, obs, h)) r.push_back(h);
    return r;
}

bool same(std::span<const EntityHandle> a, const std::vector<EntityHandle>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
bool same_hits(std::span<const RayHit> a, const std::vector<RayHit>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].handle != b[i].handle || std::bit_cast<uint32_t>(a[i].t) != std::bit_cast<uint32_t>(b[i].t)) return false;
    return true;
}

// ---------------- scene generation ----------------
struct World {
    float extent = 2000.0f;  // x/z half-size
    float height = 200.0f;   // y half-size
    float min_half = 1.0f, max_half = 40.0f;
};

Aabb random_box(Rng& r, const World& w) {
    const Vec3 c{r.range(-w.extent, w.extent), r.range(-w.height, w.height), r.range(-w.extent, w.extent)};
    // Some boxes snap to a coarse grid so equal coordinates / touching faces / equal t occur.
    Vec3 cc = c;
    if (r.chance(0.2f)) cc = Vec3{static_cast<float>(static_cast<int>(c.x / 50.0f)) * 50.0f,
                                  static_cast<float>(static_cast<int>(c.y / 50.0f)) * 50.0f,
                                  static_cast<float>(static_cast<int>(c.z / 50.0f)) * 50.0f};
    const float hx = r.chance(0.05f) ? 0.0f : r.range(w.min_half, w.max_half);  // some flat boxes
    float hy = r.range(w.min_half, w.max_half), hz = r.range(w.min_half, w.max_half);
    if (r.chance(0.03f)) hz = r.range(501.0f, 900.0f);  // oversize: root list (threshold 500)
    if (r.chance(0.01f)) hy = 500.0f;                  // exactly at the threshold: stays in the BVH
    return {{cc.x - hx, cc.y - hy, cc.z - hz}, {cc.x + hx, cc.y + hy, cc.z + hz}};
}
uint32_t random_mask(Rng& r) { return 1u << r.below(8); }
QueryFilter random_query_mask(Rng& r) {
    const uint32_t k = r.below(6);
    if (k == 0) return kAllCategories;
    if (k == 1) return random_mask(r);
    if (k == 2) return kSkipDead;  // dead-flagged entries stay linked until the flush
    if (k == 3) return QueryFilter{random_mask(r) | random_mask(r), kDeadCategory};
    if (k == 4) return QueryFilter{kAllCategories, random_mask(r)};
    return random_mask(r) | random_mask(r);
}
Vec3 random_point(Rng& r, const World& w) {
    return {r.range(-w.extent, w.extent), r.range(-w.height, w.height), r.range(-w.extent, w.extent)};
}
Frustum random_frustum(Rng& r, const World& w) {
    if (r.chance(0.25f)) {  // arbitrary half-space set
        Frustum f;
        for (Plane& p : f.planes) {
            p.n = {r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)};
            p.d = -dot(p.n, random_point(r, w)) + r.range(0.0f, 1500.0f);
        }
        return f;
    }
    const Vec3 eye = random_point(r, w);
    const Vec3 fwd{r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)};
    const Vec3 up = std::abs(fwd.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 right = cross(fwd, up);
    const Vec3 up2 = cross(right, fwd);
    const float n = r.range(1.0f, 20.0f), f = r.range(200.0f, 3000.0f), s = r.range(0.2f, 1.0f);
    Vec3 nq[4], fq[4];
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        nq[i] = eye + fwd * n + right * (sx[i] * n * s) + up2 * (sy[i] * n * s);
        fq[i] = eye + fwd * f + right * (sx[i] * f * s) + up2 * (sy[i] * f * s);
    }
    return frustum_from_corners(nq, fq);
}
Ray random_ray(Rng& r, const World& w, const Model& m) {
    Ray q;
    q.origin = random_point(r, w);
    if (!m.empty() && r.chance(0.3f)) {  // aim at an entry (equal t and corner hits)
        auto it = m.begin();
        std::advance(it, r.below(static_cast<uint32_t>(std::min<size_t>(m.size(), 64))));
        q.dir = center(it->second.box) - q.origin;
        q.max_t = r.range(0.5f, 2.0f);
    } else {
        q.dir = {r.range(-1, 1), r.chance(0.2f) ? 0.0f : r.range(-0.3f, 0.3f), r.range(-1, 1)};
        if (r.chance(0.1f)) q.dir = {0, 0, r.chance(0.5f) ? 1.0f : -1.0f};  // axis-parallel
        q.max_t = r.range(10.0f, 4000.0f);
    }
    return q;
}

// Checks every query kind against brute force: `nq` random queries.
void check_queries(const SpatialTree& t, const Model& m, Rng& r, const World& w, int nq, QueryContext& ctx) {
    for (int i = 0; i < nq; ++i) {
        const QueryFilter mask = random_query_mask(r);
        {
            const Vec3 c = random_point(r, w);
            const float h = r.range(0.0f, 600.0f);
            const Aabb q{{c.x - h, c.y - h, c.z - h}, {c.x + h, c.y + h * 0.5f, c.z + h}};
            CHECK(same(t.query_aabb(q, mask, ctx), bf_aabb(m, q, mask)));
        }
        {
            const Sphere s{random_point(r, w), r.chance(0.1f) ? 0.0f : r.range(0.0f, 800.0f)};
            CHECK(same(t.query_sphere(s, mask, ctx), bf_sphere(m, s, mask)));
            const CircleXY cq{s.center, s.radius};
            CHECK(same(t.query_circle_xy(cq, mask, ctx), bf_circle(m, cq, mask)));
        }
        {
            const Frustum f = random_frustum(r, w);
            CHECK(same(t.query_frustum(f, mask, ctx), bf_frustum(m, f, mask)));
        }
        {
            const Ray q = random_ray(r, w, m);
            const auto ref = bf_ray(m, q, mask);
            CHECK(same_hits(t.query_ray(q, mask, ctx), ref));
            const EntityHandle ignore = (!ref.empty() && r.chance(0.5f)) ? ref[0].handle : kNoEntity;
            RayHit first;
            const bool got = t.raycast_first(q, mask, ctx, first, ignore);
            const RayHit* exp = nullptr;
            for (const RayHit& hh : ref)
                if (hh.handle != ignore) {
                    exp = &hh;
                    break;
                }
            CHECK(got == (exp != nullptr));
            if (got && exp) CHECK(first.handle == exp->handle && first.t == exp->t);
        }
        {
            const Vec3 a = random_point(r, w), b = random_point(r, w);
            const EntityHandle i0 = m.empty() ? kNoEntity : m.begin()->first;
            CHECK(t.segment_blocked(a, b, mask, ctx, i0) == bf_blocked(m, a, b, mask, i0, kNoEntity));
        }
        {
            EntityHandle obs = kNoEntity;
            Vec3 eye = random_point(r, w);
            if (!m.empty() && r.chance(0.5f)) {
                auto it = m.begin();
                std::advance(it, r.below(static_cast<uint32_t>(std::min<size_t>(m.size(), 32))));
                obs = it->first;
                eye = center(it->second.box);
            }
            const Sphere v{eye, r.range(100.0f, 900.0f)};
            const QueryFilter bm = r.chance(0.5f) ? QueryFilter{random_mask(r)} : QueryFilter{random_mask(r), kDeadCategory};
            CHECK(same(t.query_visible(v, mask, bm, obs, ctx), bf_visible(m, v, mask, bm, obs)));
        }
    }
}

// ---------------- tests ----------------

void test_geometry() {
    const Aabb b{{0, 0, 0}, {10, 10, 10}};
    float t = -1;
    CHECK(ray_aabb(PreparedRay(Ray{{-10, 5, 5}, {1, 0, 0}, 100}), b, t) && t == 10.0f);
    CHECK(ray_aabb(PreparedRay(Ray{{5, 5, 5}, {1, 0, 0}, 1}), b, t) && t == 0.0f);   // inside
    CHECK(!ray_aabb(PreparedRay(Ray{{-10, 5, 5}, {1, 0, 0}, 5}), b, t));             // too short
    CHECK(!ray_aabb(PreparedRay(Ray{{-10, 11, 5}, {1, 0, 0}, 100}), b, t));          // parallel outside
    CHECK(ray_aabb(PreparedRay(Ray{{-10, 10, 5}, {1, 0, 0}, 100}), b, t));           // grazing face
    CHECK(overlaps(Sphere{{13, 14, 5}, 5.0f}, b));                                 // 3-4-5 touch
    CHECK(!overlaps(Sphere{{13, 14, 5}, 4.99f}, b));
    const Vec3 nq[4] = {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    const Vec3 fq[4] = {{-10, -10, 10}, {10, -10, 10}, {10, 10, 10}, {-10, 10, 10}};
    const Frustum f = frustum_from_corners(nq, fq);
    CHECK(overlaps(f, Aabb{{-0.5f, -0.5f, 4}, {0.5f, 0.5f, 5}}));
    CHECK(!overlaps(f, Aabb{{-0.5f, -0.5f, 11}, {0.5f, 0.5f, 12}}));
    CHECK(!overlaps(f, Aabb{{-0.5f, -0.5f, -2}, {0.5f, 0.5f, 0}}));
    CHECK(!overlaps(f, Aabb{{8, -0.5f, 4}, {9, 0.5f, 5}}));
}

void test_basic_api() {
    SpatialTree t;
    QueryContext ctx;
    CHECK(t.query_sphere({{0, 0, 0}, 1e6f}, kAllCategories, ctx).empty());
    CHECK(t.insert(5, {{0, 0, 0}, {1, 1, 1}}, 1));
    CHECK(!t.insert(5, {{0, 0, 0}, {1, 1, 1}}, 1));           // duplicate
    CHECK(!t.insert(kNoEntity, {{0, 0, 0}, {1, 1, 1}}, 1));   // reserved
    CHECK(!t.insert(6, {{1, 0, 0}, {0, 1, 1}}, 1));           // min > max
    CHECK(!t.update(5, {{0, 0, 0}, {1, 1, std::numeric_limits<float>::infinity()}}));      // not finite
    CHECK(t.insert(3, {{5, 0, 0}, {6, 1, 1}}, 2));
    CHECK(t.size() == 2 && t.validate());
    auto r = t.query_sphere({{0, 0, 0}, 100}, kAllCategories, ctx);
    CHECK(r.size() == 2 && r[0] == 3 && r[1] == 5);
    r = t.query_sphere({{0, 0, 0}, 100}, 2, ctx);
    CHECK(r.size() == 1 && r[0] == 3);
    CHECK(t.set_mask(3, 4) && t.mask_of(3) == 4 && t.validate());
    CHECK(t.query_sphere({{0, 0, 0}, 100}, 2, ctx).empty());
    CHECK(t.set_dead(5) && t.mask_of(5) == (1u | kDeadCategory) && t.validate());
    r = t.query_sphere({{0, 0, 0}, 100}, kSkipDead, ctx);
    CHECK(r.size() == 1 && r[0] == 3);
    CHECK(t.query_sphere({{0, 0, 0}, 100}, kAllCategories, ctx).size() == 2);  // dead still linked
    CHECK(!t.segment_blocked({-5, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, kSkipDead, ctx));
    CHECK(t.segment_blocked({-5, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, kAllCategories, ctx));
    CHECK(t.set_dead(5, false) && t.mask_of(5) == 1u);
    CHECK(t.remove(5) && !t.remove(5) && t.size() == 1 && t.validate());
    CHECK(t.bounds(3) && t.bounds(3)->min.x == 5.0f && !t.bounds(5));
    t.rebuild();
    CHECK(t.validate() && t.node_count() == 1 && t.height() == 1);
    CHECK(t.remove(3) && t.size() == 0 && t.validate());
}

// Random insert/remove/move/mask stream, queries checked against brute force along the way.
void test_random_streams() {
    const float margins[2] = {0.0f, 5.0f};
    int checks = 0;
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        for (float margin : margins) {
            Rng r(seed * 7919u + static_cast<uint64_t>(margin));
            World w;
            SpatialTreeConfig cfg;
            cfg.fat_margin = margin;
            cfg.parallel_threshold = 64;  // exercise the parallel rebuild path on small trees too
            SpatialTree t(cfg);
            Model m;
            QueryContext ctx;
            const auto pf = ::alamo::test::make_thread_parallel_for(4);
            double game_time = 0.0;
            const bool large_handles = (seed & 1) == 0;
            for (int op = 0; op < 6000; ++op) {
                const uint32_t k = r.below(100);
                if (k < 35 || m.size() < 50) {
                    EntityHandle h = large_handles ? static_cast<EntityHandle>(r.next()) : r.below(4000);
                    if (h == kNoEntity) h = 0;
                    const Aabb b = random_box(r, w);
                    const uint32_t mk = random_mask(r);
                    const bool ok = t.insert(h, b, mk);
                    CHECK(ok == (m.find(h) == m.end()));
                    if (ok) m[h] = {b, mk};
                } else if (k < 50) {
                    auto it = m.begin();
                    std::advance(it, r.below(static_cast<uint32_t>(m.size())));
                    CHECK(t.remove(it->first));
                    m.erase(it);
                } else if (k < 95) {
                    auto it = m.begin();
                    std::advance(it, r.below(static_cast<uint32_t>(m.size())));
                    Aabb b = it->second.box;
                    if (r.chance(0.1f)) {
                        b = random_box(r, w);  // teleport / resize
                    } else {
                        const Vec3 d{r.range(-8, 8), r.range(-2, 2), r.range(-8, 8)};
                        b = {b.min + d, b.max + d};
                    }
                    CHECK(t.update(it->first, b));
                    it->second.box = b;
                } else if (k < 96) {
                    auto it = m.begin();
                    std::advance(it, r.below(static_cast<uint32_t>(m.size())));
                    const bool dead = r.chance(0.7f);
                    CHECK(t.set_dead(it->first, dead));
                    it->second.mask = dead ? (it->second.mask | kDeadCategory) : (it->second.mask & ~kDeadCategory);
                    CHECK(t.mask_of(it->first) == it->second.mask);
                } else if (k < 97) {
                    auto it = m.begin();
                    std::advance(it, r.below(static_cast<uint32_t>(m.size())));
                    const uint32_t mk = random_mask(r);
                    CHECK(t.set_mask(it->first, mk));
                    it->second.mask = mk;
                } else {
                    game_time += 0.25;
                    t.maintain(game_time, pf);
                }
                if (op % 500 == 499) {
                    CHECK(t.validate());
                    CHECK(t.size() == m.size());
                    check_queries(t, m, r, w, 20, ctx);
                    ++checks;
                }
            }
            t.rebuild(pf);
            CHECK(t.validate());
            check_queries(t, m, r, w, 40, ctx);
        }
    }
    std::printf("random streams: %d checkpoints x 6 query kinds vs brute force\n", checks);
}

// Recorded "battle" scene: two fleets of clustered ships closing on each other at 30 Hz,
// projectiles spawning/dying, maintain() each tick (original 1 s cadence).
void test_battle_scene() {
    Rng r(0xBA77135ull);
    World w;
    w.extent = 3000.0f;
    SpatialTreeConfig cfg;
    cfg.fat_margin = 4.0f;
    SpatialTree t(cfg);
    Model m;
    QueryContext ctx;
    struct Mover {
        EntityHandle h;
        Vec3 vel;
        int ttl;
    };
    std::vector<Mover> movers;
    EntityHandle next = 1;
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < 1500; ++i) {
            const float cx = side ? 2000.0f : -2000.0f;
            const Vec3 c{cx + r.range(-400, 400), r.range(-100, 100), r.range(-1500, 1500)};
            const float hs = r.chance(0.003f) ? r.range(520, 700) : (r.chance(0.05f) ? r.range(60, 150) : r.range(3, 25));
            const Aabb b{{c.x - hs, c.y - hs * 0.5f, c.z - hs}, {c.x + hs, c.y + hs * 0.5f, c.z + hs}};
            const uint32_t mk = (side ? 2u : 1u) | (hs > 50 ? 0x100u : 0x10u);
            t.insert(next, b, mk);
            m[next] = {b, mk};
            movers.push_back({next, {side ? -3.0f : 3.0f, 0, r.range(-0.5f, 0.5f)}, -1});
            ++next;
        }
    t.rebuild();
    uint32_t rebuilds = 0;
    for (int tick = 0; tick < 300; ++tick) {
        const double gt = tick / 30.0;
        // projectiles
        for (int i = 0; i < 20; ++i) {
            const auto& src = movers[r.below(static_cast<uint32_t>(movers.size()))];
            if (src.ttl >= 0) continue;
            const Vec3 c = center(m[src.h].box);
            const Aabb b{{c.x - 1, c.y - 1, c.z - 1}, {c.x + 1, c.y + 1, c.z + 1}};
            t.insert(next, b, 0x1000u);
            m[next] = {b, 0x1000u};
            movers.push_back({next, {r.range(-40, 40), 0, r.range(-40, 40)}, 15 + static_cast<int>(r.below(30))});
            ++next;
        }
        for (size_t i = 0; i < movers.size();) {
            Mover& mv = movers[i];
            if (mv.ttl == 0) {
                t.remove(mv.h);
                m.erase(mv.h);
                movers[i] = movers.back();
                movers.pop_back();
                continue;
            }
            if (mv.ttl > 0) --mv.ttl;
            if (mv.ttl == 0) {  // dies this tick: flagged now, unlinked in next tick's flush
                t.set_dead(mv.h);
                m[mv.h].mask |= kDeadCategory;
            }
            Aabb b = m[mv.h].box;
            b = {b.min + mv.vel, b.max + mv.vel};
            t.update(mv.h, b);
            m[mv.h].box = b;
            ++i;
        }
        if (t.maintain(gt)) ++rebuilds;
        if (tick % 25 == 0) {
            CHECK(t.validate());
            check_queries(t, m, r, w, 10, ctx);
        }
    }
    std::printf("battle scene: %zu entries at end, %u cadence rebuilds in 10 s\n", m.size(), rebuilds);
    CHECK(rebuilds >= 2 && rebuilds <= 11);
}

// Same entry set reached through different histories -> identical tree after rebuild, identical
// CRC stream, identical results; serial and 1/4/N-thread rebuilds give the same tree.
void test_history_and_thread_independence() {
    Rng r(42);
    World w;
    w.extent = 8000.0f;
    std::vector<std::pair<EntityHandle, Item>> final_set;
    for (uint32_t i = 0; i < 20000; ++i) final_set.push_back({i * 3u + 7u, {random_box(r, w), random_mask(r)}});

    auto crc = [](const SpatialTree& t) {
        uint64_t h = 1469598103934665603ull;
        t.hash_state([&h](const void* p, size_t n) {
            const auto* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        });
        return h;
    };

    SpatialTreeConfig cfg;  // default threshold 16384: 20K entries engage the parallel path
    // History A: insert in order.
    SpatialTree a(cfg);
    for (const auto& [h, it] : final_set) a.insert(h, it.box, it.mask);
    // History B: reverse order, plus junk inserted/moved/removed, plus wrong boxes fixed later.
    SpatialTree b(cfg);
    for (size_t i = final_set.size(); i-- > 0;) {
        const auto& [h, it] = final_set[i];
        b.insert(h, random_box(r, w), it.mask);
        if (i % 7 == 0) b.insert(1000000u + static_cast<uint32_t>(i), random_box(r, w), 1);
    }
    b.rebuild();
    for (size_t i = 0; i < final_set.size(); ++i) {
        if (i % 7 == 0) b.remove(1000000u + static_cast<uint32_t>(i));
        b.update(final_set[i].first, final_set[i].second.box);
    }
    CHECK(crc(a) == crc(b));
    a.rebuild();
    b.rebuild();
    CHECK(a.validate() && b.validate());
    CHECK(a.structure_digest() == b.structure_digest());
    const uint64_t serial_digest = a.structure_digest();
    for (unsigned threads : {1u, 4u, std::max(2u, std::thread::hardware_concurrency())}) {
        const auto pf = ::alamo::test::make_thread_parallel_for(threads);
        for (int rep = 0; rep < 3; ++rep) {
            b.rebuild(pf);
            CHECK(b.validate());
            CHECK(b.structure_digest() == serial_digest);
        }
    }
    // A change in one box changes the CRC.
    const uint64_t before = crc(a);
    Aabb moved = final_set[5].second.box;
    moved.max.x += 0.5f;
    a.update(final_set[5].first, moved);
    CHECK(crc(a) != before);
    std::printf("history/thread independence: 20000 entries, digest %016llx\n",
                static_cast<unsigned long long>(serial_digest));
}

// Batches through 1/4/N threads == serial == single queries.
void test_batch_determinism() {
    Rng r(777);
    World w;
    w.extent = 6000.0f;
    SpatialTreeConfig cfg;
    SpatialTree t(cfg);
    Model m;
    for (uint32_t i = 0; i < 17000; ++i) {
        const Aabb b = random_box(r, w);
        const uint32_t mk = random_mask(r);
        t.insert(i, b, mk);
        m[i] = {b, mk};
    }
    t.rebuild();
    std::vector<BatchQuery> qs(600);
    for (size_t i = 0; i < qs.size(); ++i) {
        BatchQuery& q = qs[i];
        q.kind = static_cast<QueryKind>(i % 6);
        q.filter = random_query_mask(r);
        const Vec3 c = random_point(r, w);
        q.box = {c - Vec3{300, 300, 300}, c + Vec3{300, 300, 300}};
        q.sphere = {c, r.range(100, 700)};
        q.circle = {c, r.range(100, 700)};
        q.frustum = random_frustum(r, w);
        q.ray = random_ray(r, w, m);
        q.blockers = QueryFilter{random_mask(r), kDeadCategory};
        q.observer = r.below(17000);
    }
    BatchResults serial;
    t.query_batch(qs, serial);  // serial_for: same code path, no threads
    QueryContext ctx;
    for (size_t i = 0; i < qs.size(); ++i) {
        t.run_query(qs[i], ctx);
        CHECK(same(serial.handles(i), std::vector<EntityHandle>(ctx.results.begin(), ctx.results.end())));
        CHECK(same_hits(serial.hits(i), std::vector<RayHit>(ctx.hits.begin(), ctx.hits.end())));
    }
    // brute force spot check of the batch
    for (size_t i = 0; i < qs.size(); i += 37) {
        const BatchQuery& q = qs[i];
        switch (q.kind) {
            case QueryKind::Aabb: CHECK(same(serial.handles(i), bf_aabb(m, q.box, q.filter))); break;
            case QueryKind::Sphere: CHECK(same(serial.handles(i), bf_sphere(m, q.sphere, q.filter))); break;
            case QueryKind::Frustum: CHECK(same(serial.handles(i), bf_frustum(m, q.frustum, q.filter))); break;
            case QueryKind::CircleXY: CHECK(same(serial.handles(i), bf_circle(m, q.circle, q.filter))); break;
            case QueryKind::Ray: CHECK(same_hits(serial.hits(i), bf_ray(m, q.ray, q.filter))); break;
            case QueryKind::Visible:
                CHECK(same(serial.handles(i), bf_visible(m, q.sphere, q.filter, q.blockers, q.observer)));
                break;
        }
    }
    for (unsigned threads : {1u, 4u, std::max(2u, std::thread::hardware_concurrency())}) {
        BatchResults par;
        t.query_batch(qs, par, ::alamo::test::make_thread_parallel_for(threads));
        for (size_t i = 0; i < qs.size(); ++i) {
            CHECK(same(par.handles(i), std::vector<EntityHandle>(serial.handles(i).begin(), serial.handles(i).end())));
            CHECK(same_hits(par.hits(i), std::vector<RayHit>(serial.hits(i).begin(), serial.hits(i).end())));
        }
    }
}

// Steady state: queries, batches, update() and rebuild() perform zero heap allocations.
void test_zero_allocations() {
    Rng r(2024);
    World w;
    w.extent = 5000.0f;
    SpatialTreeConfig cfg;
    cfg.fat_margin = 2.0f;
    SpatialTree t(cfg);
    Model m;
    for (uint32_t i = 0; i < 5000; ++i) {
        const Aabb b = random_box(r, w);
        t.insert(i * 11u, b, random_mask(r));
        m[i * 11u] = {b, 0};
    }
    t.rebuild();
    std::vector<BatchQuery> qs(256);
    std::vector<Sphere> spheres;
    std::vector<Aabb> boxes;
    std::vector<Frustum> frusta;
    std::vector<Ray> rays;
    for (size_t i = 0; i < qs.size(); ++i) {
        const Vec3 c = random_point(r, w);
        spheres.push_back({c, r.range(50, 900)});
        boxes.push_back({c - Vec3{400, 400, 400}, c + Vec3{400, 400, 400}});
        frusta.push_back(random_frustum(r, w));
        rays.push_back(random_ray(r, w, m));
        qs[i].kind = static_cast<QueryKind>(i % 6);
        qs[i].circle = {c, 700.0f};
        qs[i].sphere = spheres.back();
        qs[i].box = boxes.back();
        qs[i].frustum = frusta.back();
        qs[i].ray = rays.back();
        qs[i].blockers = QueryFilter{1u};
    }
    std::vector<Aabb> moved(5000);
    for (uint32_t i = 0; i < 5000; ++i) {
        const Aabb& b = m[i * 11u].box;
        const Vec3 d{r.range(-30, 30), 0, r.range(-30, 30)};
        moved[i] = {b.min + d, b.max + d};
    }
    QueryContext ctx;
    BatchResults batch;
    size_t sink = 0;
    auto run_all = [&] {
        for (size_t i = 0; i < qs.size(); ++i) {
            sink += t.query_sphere(spheres[i], kAllCategories, ctx).size();
            sink += t.query_circle_xy(CircleXY{spheres[i].center, spheres[i].radius}, kSkipDead, ctx).size();
            sink += t.query_aabb(boxes[i], 3, ctx).size();
            sink += t.query_frustum(frusta[i], kAllCategories, ctx).size();
            sink += t.query_ray(rays[i], kAllCategories, ctx).size();
            RayHit h;
            sink += t.raycast_first(rays[i], kAllCategories, ctx, h) ? 1 : 0;
            sink += t.segment_blocked(spheres[i].center, boxes[i].max, 0xFF, ctx) ? 1 : 0;
            sink += t.query_visible(spheres[i], kAllCategories, 1, kNoEntity, ctx).size();
        }
        t.query_batch(qs, batch);
        for (uint32_t i = 0; i < 5000; ++i) t.update(i * 11u, moved[i]);
        t.rebuild();
    };
    // Warm-up: contexts and rebuild scratch reach their steady capacity. Twice, because the
    // first pass moves the entries, so the second sees the final scene.
    run_all();
    run_all();
    const uint64_t before = g_allocs.load();
    run_all();
    run_all();
    const uint64_t allocs = g_allocs.load() - before;
    std::printf("zero-alloc: %llu allocations over 2 x (%zu x 7 queries + %zu-query batch + 5000 updates + rebuild) "
                "(sink %zu)\n",
                static_cast<unsigned long long>(allocs), qs.size(), qs.size(), sink);
    CHECK(allocs == 0);
}

// Oversize entries (largest half-extent > threshold) live in the root list; map-load threshold
// changes reclassify on rebuild; results stay identical to brute force.
void test_oversize() {
    SpatialTree t;
    QueryContext ctx;
    CHECK(t.insert(1, {{0, 0, 0}, {10, 10, 10}}, 1));
    CHECK(t.insert(2, {{-1000, -5, -5}, {1000, 5, 5}}, 1));  // half 1000 > 500
    CHECK(t.insert(3, {{-500, 0, 0}, {500, 1, 1}}, 1));      // half exactly 500: not oversize
    CHECK(!t.is_oversize(1) && t.is_oversize(2) && !t.is_oversize(3) && t.oversize_count() == 1);
    CHECK(t.validate());
    auto r = t.query_sphere({{900, 0, 0}, 1}, kAllCategories, ctx);
    CHECK(r.size() == 1 && r[0] == 2);
    CHECK(t.update(2, {{0, 0, 0}, {5, 5, 5}}) && !t.is_oversize(2) && t.validate());  // shrinks into the BVH
    CHECK(t.update(1, {{0, 0, 0}, {2000, 1, 1}}) && t.is_oversize(1) && t.validate());  // grows out
    t.rebuild_with_threshold(2.0f);
    CHECK(t.is_oversize(1) && t.is_oversize(2) && t.is_oversize(3) && t.validate());
    RayHit h;
    CHECK(t.raycast_first(Ray{{-10, 0.5f, 0.5f}, {1, 0, 0}, 100}, kAllCategories, ctx, h) && h.handle == 3 && h.t == 0.0f);
    CHECK(t.segment_blocked({1, 0.5f, 0.5f}, {1, 0.5f, 3}, kAllCategories, ctx, 2));
    t.rebuild_with_threshold(500.0f);
    CHECK(t.is_oversize(1) && !t.is_oversize(2) && !t.is_oversize(3) && t.validate());
    // random scene with a low threshold: many entries in the root list
    Rng rg(99);
    World w;
    SpatialTreeConfig cfg;
    cfg.large_threshold = 30.0f;
    SpatialTree u(cfg);
    Model m;
    for (uint32_t i = 1; i <= 3000; ++i) {
        const Aabb b = random_box(rg, w);
        const uint32_t mk = random_mask(rg);
        u.insert(i, b, mk);
        m[i] = {b, mk};
    }
    u.rebuild();
    CHECK(u.validate() && u.oversize_count() > 100);
    check_queries(u, m, rg, w, 40, ctx);
}

// TreeSet: per-player / all / obstacle / space trees follow the membership rules; owner change
// moves the object between player trees; every tree's queries match brute force over its model.
void test_tree_set() {
    Rng r(4242);
    World w;
    const uint32_t players = 4;
    TreeSet ts(players);
    struct Obj {
        Aabb box;
        uint32_t mask;
        TreeMembership m;
    };
    std::map<EntityHandle, Obj> objs;
    EntityHandle next_id = 1;  // object ids: shared counter from 1, never reused
    auto model_of = [&](int tree) {  // 0 all, 1..P players, P+1 obstacle, P+2 space
        Model md;
        for (const auto& [id, o] : objs) {
            bool in = false;
            if (tree == 0) in = o.m.projectile_collidable;
            else if (tree <= static_cast<int>(players)) in = o.m.projectile_collidable && o.m.owner == tree - 1;
            else if (tree == static_cast<int>(players) + 1) in = o.m.obstacle;
            else in = o.m.space_avoidance;
            if (in) md[id] = {o.box, o.mask};
        }
        return md;
    };
    auto tree_of = [&](int tree) -> const SpatialTree& {
        if (tree == 0) return ts.all();
        if (tree <= static_cast<int>(players)) return ts.player(static_cast<uint32_t>(tree - 1));
        if (tree == static_cast<int>(players) + 1) return ts.obstacle();
        return ts.space_avoidance();
    };
    auto random_membership = [&] {
        TreeMembership m;
        m.owner = static_cast<int32_t>(r.below(players + 1)) - 1;  // -1 = neutral
        m.projectile_collidable = r.chance(0.8f);
        m.obstacle = r.chance(0.3f);
        m.space_avoidance = r.chance(0.5f);
        return m;
    };
    QueryContext ctx;
    double gt = 0.0;
    for (int op = 0; op < 4000; ++op) {
        const uint32_t k = r.below(100);
        if (k < 30 || objs.size() < 20) {
            const Obj o{random_box(r, w), random_mask(r), random_membership()};
            CHECK(ts.add(next_id, o.box, o.mask, o.m));
            objs[next_id++] = o;
        } else {
            auto it = objs.begin();
            std::advance(it, r.below(static_cast<uint32_t>(objs.size())));
            if (k < 40) {
                CHECK(ts.remove(it->first));
                objs.erase(it);
            } else if (k < 80) {
                const Vec3 d{r.range(-10, 10), r.range(-10, 10), r.range(-2, 2)};
                it->second.box = {it->second.box.min + d, it->second.box.max + d};
                CHECK(ts.update(it->first, it->second.box));
            } else if (k < 88) {  // ObjectOwnerChanged
                const int32_t owner = static_cast<int32_t>(r.below(players + 1)) - 1;
                CHECK(ts.set_owner(it->first, owner));
                it->second.m.owner = owner;
            } else if (k < 93) {  // living -> dead collidability etc.
                it->second.m = random_membership();
                CHECK(ts.set_membership(it->first, it->second.m));
            } else if (k < 97) {
                CHECK(ts.set_dead(it->first, true));
                it->second.mask |= kDeadCategory;
            } else {
                gt += 0.3;
                ts.maintain(gt);
            }
        }
        if (op % 400 == 399) {
            for (int tr = 0; tr <= static_cast<int>(players) + 2; ++tr) {
                const Model md = model_of(tr);
                const SpatialTree& t = tree_of(tr);
                CHECK(t.size() == md.size() && t.validate());
                check_queries(t, md, r, w, 4, ctx);
            }
            CHECK(&ts.for_player(-1) == &ts.all() && &ts.for_player(2) == &ts.player(2));
        }
    }
    uint64_t h1 = 0;
    ts.hash_state([&h1](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h1 = (h1 ^ b[i]) * 1099511628211ull;
    });
    CHECK(h1 != 0);
    std::printf("tree set: %zu objects across %u player trees + all/obstacle/space\n", objs.size(), players);
}

void test_maintain_cadence() {    SpatialTreeConfig cfg;  // 1 s, 20%
    SpatialTree t(cfg);
    for (uint32_t i = 0; i < 100; ++i) t.insert(i, {{float(i) * 10, 0, 0}, {float(i) * 10 + 1, 1, 1}}, 1);
    CHECK(t.dirty());
    CHECK(t.maintain(0.0));
    CHECK(!t.dirty() && !t.maintain(0.1));
    for (uint32_t i = 0; i < 20; ++i) t.update(i, {{float(i) * 10, 50, 0}, {float(i) * 10 + 1, 51, 1}});
    CHECK(!t.dirty());  // exactly 20% is not "more than 20%"
    t.update(20, {{200, 50, 0}, {201, 51, 1}});
    CHECK(t.dirty());
    CHECK(!t.maintain(0.5));  // dirty, but < 1 s since the last rebuild
    CHECK(t.maintain(1.0));
    CHECK(!t.dirty() && t.validate());
}

}  // namespace

int main() {
    test_geometry();
    test_basic_api();
    test_maintain_cadence();
    test_oversize();
    test_tree_set();
    test_random_streams();
    test_battle_scene();
    test_history_and_thread_independence();
    test_batch_determinism();
    test_zero_allocations();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_tactical_tests: all passed\n");
    return 0;
}
