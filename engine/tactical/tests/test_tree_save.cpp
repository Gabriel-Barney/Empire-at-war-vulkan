// alamo_tactical_tree_save_tests (no framework; non-zero exit on failure). N1 stage 2: SpatialTree / TreeSet save and
// load (spatial_tree.hpp / tree_set.hpp "N1 stage 2"). The trees are saved as their entry sets and rebuilt on load, so
// the loaded tree's SHAPE differs from the saving tree's; these tests prove that nothing observable does:
//   * a tree with a long insert / update / remove / mask / dead / maintain / threshold history and its loaded copy give
//     identical hash_state() streams and identical results for every query kind (AABB, sphere, 2-D circle, frustum,
//     ray hits with t, raycast_first, segment_blocked, visible, batches), and stay identical while both keep being
//     mutated with the same operations afterwards;
//   * the same entry set inserted in another order and rebuilt writes the same bytes; write -> read -> write is
//     byte-identical;
//   * every truncation and the detectable corruptions (version, order, reserved handle, invalid box, counts, player
//     count) are rejected with the tree / set unchanged.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/tactical/spatial_tree.hpp"
#include "alamo/tactical/tree_set.hpp"

using namespace alamo;
using namespace alamo::tactical;

namespace {


// splitmix64 (test data only).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<uint32_t>((z ^ (z >> 31)) >> 32);
    }
    uint32_t below(uint32_t n) { return n == 0 ? 0 : next() % n; }
    float range(float a, float b) { return a + (b - a) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

Aabb box_at(Vec3 c, float hx, float hy, float hz) { return Aabb{{c.x - hx, c.y - hy, c.z - hz}, {c.x + hx, c.y + hy, c.z + hz}}; }

Aabb random_box(Rng& r) {
    const Vec3 c{r.range(-4000.0f, 4000.0f), r.range(-4000.0f, 4000.0f), r.range(-200.0f, 200.0f)};
    if (r.below(20) == 0) return box_at(c, r.range(450.0f, 900.0f), r.range(10.0f, 700.0f), r.range(5.0f, 50.0f));  // oversize
    return box_at(c, r.range(1.0f, 120.0f), r.range(1.0f, 120.0f), r.range(1.0f, 40.0f));
}

std::vector<uint8_t> state_bytes(const SpatialTree& t) {
    std::vector<uint8_t> out;
    t.hash_state([&out](const void* d, size_t n) {
        const auto* p = static_cast<const uint8_t*>(d);
        out.insert(out.end(), p, p + n);
    });
    return out;
}

std::vector<uint8_t> state_bytes(const TreeSet& t) {
    std::vector<uint8_t> out;
    t.hash_state([&out](const void* d, size_t n) {
        const auto* p = static_cast<const uint8_t*>(d);
        out.insert(out.end(), p, p + n);
    });
    return out;
}

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

uint32_t get_u32(const std::vector<uint8_t>& v, size_t at) {
    uint32_t x = 0;
    for (size_t i = 0; i < 4; ++i) x |= static_cast<uint32_t>(v[at + i]) << (8 * i);
    return x;
}

QueryFilter random_filter(Rng& r) {
    switch (r.below(4)) {
        case 0: return QueryFilter{};
        case 1: return kSkipDead;
        case 2: return QueryFilter{0x5u, kDeadCategory};
        default: return QueryFilter{1u << r.below(4), 0u};
    }
}

Frustum box_frustum(Vec3 c, float h) {
    const Vec3 nq[4] = {{c.x - h, c.y - h, c.z - h}, {c.x + h, c.y - h, c.z - h}, {c.x + h, c.y + h, c.z - h}, {c.x - h, c.y + h, c.z - h}};
    const Vec3 fq[4] = {{c.x - h, c.y - h, c.z + h}, {c.x + h, c.y - h, c.z + h}, {c.x + h, c.y + h, c.z + h}, {c.x - h, c.y + h, c.z + h}};
    return frustum_from_corners(nq, fq);
}

// Every query kind with seeded random parameters; the results (handles, t bit patterns, bools) as bytes.
std::vector<uint8_t> query_digest(const SpatialTree& t, uint64_t seed, uint32_t count) {
    Rng r(seed);
    QueryContext ctx;
    std::vector<uint8_t> out;
    std::vector<BatchQuery> batch;
    for (uint32_t q = 0; q < count; ++q) {
        const Vec3 c{r.range(-4500.0f, 4500.0f), r.range(-4500.0f, 4500.0f), r.range(-250.0f, 250.0f)};
        const float rad = r.range(10.0f, 1500.0f);
        const QueryFilter f = random_filter(r);
        const uint32_t kind = q % 9;
        put_u32(out, 0xABCD0000u | kind);
        if (kind == 0) {
            for (EntityHandle h : t.query_aabb(box_at(c, rad, r.range(10.0f, 1500.0f), 300.0f), f, ctx)) put_u32(out, h);
        } else if (kind == 1) {
            for (EntityHandle h : t.query_sphere(Sphere{c, rad}, f, ctx)) put_u32(out, h);
        } else if (kind == 2) {
            for (EntityHandle h : t.query_circle_xy(CircleXY{c, rad}, f, ctx)) put_u32(out, h);
        } else if (kind == 3) {
            for (EntityHandle h : t.query_frustum(box_frustum(c, rad), f, ctx)) put_u32(out, h);
        } else if (kind == 4 || kind == 5) {
            const Vec3 d{r.range(-6000.0f, 6000.0f), r.range(-6000.0f, 6000.0f), r.range(-100.0f, 100.0f)};
            const Ray ray{c, d, 1.0f};
            if (kind == 4) {
                for (const RayHit& hit : t.query_ray(ray, f, ctx)) {
                    put_u32(out, hit.handle);
                    put_u32(out, std::bit_cast<uint32_t>(hit.t));
                }
            } else {
                RayHit hit;
                const EntityHandle ignore = r.below(2) == 0 ? kNoEntity : r.below(800);
                const bool found = t.raycast_first(ray, f, ctx, hit, ignore);
                put_u32(out, found ? 1u : 0u);
                if (found) {
                    put_u32(out, hit.handle);
                    put_u32(out, std::bit_cast<uint32_t>(hit.t));
                }
            }
        } else if (kind == 6) {
            const Vec3 b{r.range(-4500.0f, 4500.0f), r.range(-4500.0f, 4500.0f), r.range(-250.0f, 250.0f)};
            put_u32(out, t.segment_blocked(c, b, f, ctx, r.below(800), r.below(800)) ? 1u : 0u);
        } else if (kind == 7) {
            for (EntityHandle h : t.query_visible(Sphere{c, rad}, f, QueryFilter{0x2u, kDeadCategory}, r.below(800), ctx))
                put_u32(out, h);
        } else {
            BatchQuery bq;
            bq.kind = static_cast<QueryKind>(r.below(6));
            bq.filter = f;
            bq.blockers = QueryFilter{0x2u};
            bq.observer = r.below(800);
            bq.box = box_at(c, rad, rad, 200.0f);
            bq.sphere = Sphere{c, rad};
            bq.circle = CircleXY{c, rad};
            bq.frustum = box_frustum(c, rad);
            bq.ray = Ray{c, Vec3{r.range(-5000.0f, 5000.0f), r.range(-5000.0f, 5000.0f), 0.0f}, 1.0f};
            batch.push_back(bq);
        }
    }
    BatchResults br;
    t.query_batch(batch, br);
    for (size_t i = 0; i < batch.size(); ++i) {
        for (EntityHandle h : br.handles(i)) put_u32(out, h);
        for (const RayHit& hit : br.hits(i)) {
            put_u32(out, hit.handle);
            put_u32(out, std::bit_cast<uint32_t>(hit.t));
        }
    }
    return out;
}

std::vector<uint8_t> saved(const SpatialTree& t) {
    ByteWriter w;
    t.write(w);
    return w.bytes();
}
std::vector<uint8_t> saved(const TreeSet& t) {
    ByteWriter w;
    t.write(w);
    return w.bytes();
}

// Reads `bytes` like a save section would (read() true and every byte consumed).
template <class T>
bool load(T& t, const std::vector<uint8_t>& bytes) {
    ByteReader r(bytes);
    return t.read(r) && r.ok() && r.done();
}

// One scripted history applied identically to several trees (live handles kept ascending outside the trees).
struct TreeScript {
    Rng r;
    std::vector<EntityHandle> live;
    EntityHandle next = 1;
    double time = 0.0;
    explicit TreeScript(uint64_t seed) : r(seed) {}

    void step(std::vector<SpatialTree*> trees) {
        const uint32_t op = r.below(100);
        if (op < 18 || live.size() < 50) {  // insert
            const EntityHandle h = next;
            next += 1 + r.below(3);  // ids are never reused, gaps like the GOM counter
            const Aabb b = random_box(r);
            const uint32_t mask = 1u << r.below(4);
            for (SpatialTree* t : trees) CHECK(t->insert(h, b, mask));
            live.push_back(h);
        } else if (op < 26) {  // remove
            const size_t i = r.below(static_cast<uint32_t>(live.size()));
            for (SpatialTree* t : trees) CHECK(t->remove(live[i]));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (op < 80) {  // update: small moves stay in the fat box, big ones leave it / change size class
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const Aabb* cur = trees[0]->bounds(h);
            Aabb b = *cur;
            if (r.below(4) == 0) {
                b = random_box(r);
            } else {
                const float dx = r.range(-6.0f, 6.0f), dy = r.range(-6.0f, 6.0f);
                b.min.x += dx;
                b.max.x += dx;
                b.min.y += dy;
                b.max.y += dy;
            }
            for (SpatialTree* t : trees) CHECK(t->update(h, b));
        } else if (op < 86) {  // mask
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const uint32_t mask = (1u << r.below(4)) | (trees[0]->mask_of(h) & kDeadCategory);
            for (SpatialTree* t : trees) CHECK(t->set_mask(h, mask));
        } else if (op < 92) {  // dead flag
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const bool d = (trees[0]->mask_of(h) & kDeadCategory) == 0;
            for (SpatialTree* t : trees) CHECK(t->set_dead(h, d));
        } else if (op < 99) {  // maintain at the game time
            time += 0.25;
            for (SpatialTree* t : trees) t->maintain(time);
        } else {  // map-load threshold change
            const float lt = r.below(2) == 0 ? 300.0f : 500.0f;
            for (SpatialTree* t : trees) t->rebuild_with_threshold(lt);
        }
    }
};

void test_spatial_tree() {
    SpatialTree a;
    TreeScript s(42);
    for (int i = 0; i < 4000; ++i) s.step({&a});
    CHECK(a.validate() && a.size() > 200 && a.oversize_count() > 0);
    const std::vector<uint8_t> bytes = saved(a);

    // Load into a tree that already has other content: read() replaces it.
    SpatialTree b;
    for (EntityHandle h = 9000; h < 9010; ++h) b.insert(h, box_at({0, 0, 0}, 5, 5, 5), 1u);
    CHECK(load(b, bytes));
    CHECK(b.validate() && b.size() == a.size() && b.oversize_count() == a.oversize_count());
    CHECK(state_bytes(a) == state_bytes(b));
    CHECK(query_digest(a, 7, 900) == query_digest(b, 7, 900));
    CHECK(saved(b) == bytes);  // write -> read -> write
    std::printf("spatial tree: %u entries (%u oversize), %zu save bytes, shape digest saving %016llx loaded %016llx\n",
                a.size(), a.oversize_count(), bytes.size(), static_cast<unsigned long long>(a.structure_digest()),
                static_cast<unsigned long long>(b.structure_digest()));

    // The same entry set inserted in another (descending) order and rebuilt: same bytes, same results.
    {
        SpatialTree c;
        std::vector<EntityHandle> ids = s.live;
        std::sort(ids.begin(), ids.end(), [](EntityHandle x, EntityHandle y) { return x > y; });
        for (EntityHandle h : ids) c.insert(h, *a.bounds(h), a.mask_of(h));
        c.rebuild_with_threshold(a.config().large_threshold);
        CHECK(saved(c) == bytes && query_digest(c, 9, 300) == query_digest(a, 9, 300));
    }

    // Both keep running the same history: identical state and results at every check.
    bool same = true;
    for (int i = 0; i < 3000; ++i) {
        s.step({&a, &b});
        if (i % 60 == 0)
            same = same && state_bytes(a) == state_bytes(b) && query_digest(a, static_cast<uint64_t>(i), 90) == query_digest(b, static_cast<uint64_t>(i), 90);
    }
    CHECK(same && saved(a) == saved(b) && b.validate());

    // Truncation: every prefix is rejected, the target unchanged.
    SpatialTree d;
    for (EntityHandle h = 1; h < 40; ++h) d.insert(h * 7, box_at({static_cast<float>(h) * 10.0f, 0, 0}, 4, 4, 4), 1u);
    const std::vector<uint8_t> d0 = saved(d);
    bool rejected = true;
    for (size_t n = 0; n < bytes.size(); ++n) {
        const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        ByteReader r(cut);
        if (d.read(r)) rejected = false;
    }
    CHECK(rejected && saved(d) == d0);
    // Trailing bytes: read() itself succeeds but the section is not consumed (the owner rejects it).
    {
        std::vector<uint8_t> extra = bytes;
        extra.push_back(0);
        SpatialTree e;
        CHECK(!load(e, extra));
    }
    // Detectable corruption: version, handle order, reserved handle, invalid box, count, NaN threshold.
    const auto corrupt = [&](size_t at, uint32_t v) {
        std::vector<uint8_t> x = bytes;
        for (int i = 0; i < 4; ++i) x[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        ByteReader r(x);
        return !d.read(r) && saved(d) == d0;
    };
    const size_t first = 12, second = first + 32;  // entry 0 / entry 1 (version, threshold, count before)
    CHECK(corrupt(0, 2));                                               // version
    CHECK(corrupt(4, 0x7FC00000u));                                     // NaN threshold
    CHECK(corrupt(8, 0x7FFFFFFFu));                                     // count past the data
    CHECK(corrupt(second, get_u32(bytes, first)));                    // duplicate handle (not ascending)
    CHECK(corrupt(first, kNoEntity));                                   // reserved handle
    CHECK(corrupt(first + 8, std::bit_cast<uint32_t>(1e30f)));          // min.x > max.x
    CHECK(corrupt(first + 8, 0x7F800000u));                             // inf
    // An empty tree round-trips.
    SpatialTree empty, empty2;
    CHECK(load(empty2, saved(empty)) && empty2.size() == 0 && saved(empty2) == saved(empty));
}

// ---------------------------------------------------------------- TreeSet
struct SetScript {
    Rng r;
    std::vector<EntityHandle> live;
    EntityHandle next = 1;
    double time = 0.0;
    explicit SetScript(uint64_t seed) : r(seed) {}

    TreeMembership membership() {
        TreeMembership m;
        m.owner = static_cast<int32_t>(r.below(6)) - 1;  // -1 .. 4 (4: outside the 4 player trees)
        m.projectile_collidable = r.below(5) != 0;
        m.obstacle = r.below(4) == 0;
        m.space_avoidance = r.below(3) == 0;
        return m;
    }

    void step(std::vector<TreeSet*> sets) {
        const uint32_t op = r.below(100);
        if (op < 15 || live.size() < 40) {
            const EntityHandle h = next;
            next += 1 + r.below(2);
            const Aabb b = random_box(r);
            const uint32_t mask = 1u << r.below(4);
            const TreeMembership m = membership();
            for (TreeSet* t : sets) CHECK(t->add(h, b, mask, m));
            live.push_back(h);
        } else if (op < 22) {
            const size_t i = r.below(static_cast<uint32_t>(live.size()));
            for (TreeSet* t : sets) CHECK(t->remove(live[i]));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (op < 70) {
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const float dx = r.range(-8.0f, 8.0f);
            Aabb b = r.below(5) == 0 ? random_box(r) : box_at({dx * 100.0f, dx * 50.0f, 0.0f}, 20.0f, 20.0f, 5.0f);
            for (TreeSet* t : sets) CHECK(t->update(h, b));
        } else if (op < 78) {
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const int32_t owner = static_cast<int32_t>(r.below(5)) - 1;
            for (TreeSet* t : sets) CHECK(t->set_owner(h, owner));
        } else if (op < 84) {
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const TreeMembership m = membership();
            for (TreeSet* t : sets) CHECK(t->set_membership(h, m));
        } else if (op < 90) {
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const bool d = r.below(2) == 0;
            for (TreeSet* t : sets) CHECK(t->set_dead(h, d));
        } else if (op < 94) {
            const EntityHandle h = live[r.below(static_cast<uint32_t>(live.size()))];
            const uint32_t mask = 1u << r.below(4);
            for (TreeSet* t : sets) CHECK(t->set_mask(h, mask));
        } else {
            time += 0.4;
            for (TreeSet* t : sets) t->maintain(time);
        }
    }
};

std::vector<uint8_t> set_queries(const TreeSet& t, uint64_t seed) {
    std::vector<uint8_t> out;
    const auto add = [&out](const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); };
    add(query_digest(t.all(), seed, 60));
    for (uint32_t p = 0; p < t.player_count(); ++p) add(query_digest(t.player(p), seed + p + 1, 40));
    add(query_digest(t.for_player(-1), seed + 10, 20));
    add(query_digest(t.obstacle(), seed + 11, 40));
    add(query_digest(t.space_avoidance(), seed + 12, 40));
    return out;
}

void test_tree_set() {
    TreeSet a(4);
    SetScript s(1234);
    for (int i = 0; i < 2500; ++i) s.step({&a});
    a.rebuild_all_with_threshold(450.0f);  // map-load threshold (kept by the save)
    for (int i = 0; i < 300; ++i) s.step({&a});
    const std::vector<uint8_t> bytes = saved(a);
    TreeSet b(4);
    b.add(77777, box_at({1, 1, 1}, 1, 1, 1), 1u, TreeMembership{});
    CHECK(load(b, bytes));
    CHECK(b.size() == a.size() && state_bytes(a) == state_bytes(b) && set_queries(a, 3) == set_queries(b, 3));
    CHECK(saved(b) == bytes && b.all().config().large_threshold == 450.0f);
    for (EntityHandle h : s.live) {
        const TreeMembership* ma = a.membership(h);
        const TreeMembership* mb = b.membership(h);
        CHECK(ma && mb && ma->owner == mb->owner && ma->projectile_collidable == mb->projectile_collidable &&
              ma->obstacle == mb->obstacle && ma->space_avoidance == mb->space_avoidance);
    }
    std::printf("tree set: %u objects, %zu save bytes\n", a.size(), bytes.size());
    bool same = true;
    for (int i = 0; i < 2000; ++i) {
        s.step({&a, &b});
        if (i % 80 == 0) same = same && state_bytes(a) == state_bytes(b) && set_queries(a, static_cast<uint64_t>(i)) == set_queries(b, static_cast<uint64_t>(i));
    }
    CHECK(same && saved(a) == saved(b));

    // Rejections leave the set unchanged: other player count, every truncation, bad flags / order / box.
    TreeSet c(4);
    c.add(5, box_at({0, 0, 0}, 3, 3, 3), 1u, TreeMembership{});
    const std::vector<uint8_t> c0 = saved(c), cs0 = state_bytes(c);
    TreeSet three(3);
    CHECK(!load(three, bytes) && three.size() == 0);
    bool rejected = true;
    for (size_t n = 0; n < bytes.size(); n += (n < 256 ? 1 : 7)) {
        const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        ByteReader r(cut);
        if (c.read(r)) rejected = false;
    }
    {
        const std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
        ByteReader r(cut);
        if (c.read(r)) rejected = false;
    }
    CHECK(rejected && saved(c) == c0 && state_bytes(c) == cs0);
    const size_t objs = 8 + 4 * (4 + 3) + 4;  // version, players, thresholds, count
    const size_t stride = 4 + 24 + 4 + 4 + 1;
    const auto corrupt = [&](size_t at, uint32_t v, size_t len) {
        std::vector<uint8_t> x = bytes;
        for (size_t i = 0; i < len; ++i) x[at + i] = static_cast<uint8_t>(v >> (8 * i));
        ByteReader r(x);
        return !c.read(r) && saved(c) == c0;
    };
    CHECK(corrupt(objs + stride - 1, 8, 1));                     // unknown flag bit
    CHECK(corrupt(objs + stride, get_u32(bytes, objs), 4));      // duplicate id (not ascending)
    CHECK(corrupt(objs + 4 + 3, 0x7F, 1));                       // min.x huge / inf / NaN -> invalid box
    CHECK(corrupt(0, 9, 4));                                     // version
    CHECK(corrupt(8, 0x7FC00000u, 4));                           // NaN threshold
}

}  // namespace

int main() {
    test_spatial_tree();
    test_tree_set();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all tree save tests passed\n");
    return 0;
}
