// S6 - spatial tree replacing the original TreeCullClass (docs/re/TACTICAL_COMBAT.md section 1,
// docs/re/HOTSPOTS.md section 3): one structure for render culling and gameplay proximity queries
// (collision, weapon targeting, AI threat checks, line of sight). See tree_set.hpp for the set of
// trees the game keeps (per player, all objects, obstacle, space avoidance).
//
// HANDLES: the tree key must be the gameplay OBJECT ID (OBJECT_RUNTIME.md / TACTICAL_COMBAT.md
// 1.5: the GOM counter at +0x620, starting at 1, never reused, skips 0x3ffffe) - not a store slot
// or pointer. Every result is sorted by this key, which is what makes "first found wins" callers
// (target acquisition, area-damage victim order and caps, first projectile hit) deterministic
// (TACTICAL_COMBAT.md 0.1). 0xFFFFFFFF (kNoEntity) is reserved.
//
// Structure: a binary BVH in a node pool indexed by int32, one entry per leaf, plus a flat "root
// list" of oversize entries. Entries carry a tight AABB and a 32-bit category mask; every node
// stores the OR of its subtree's masks so category-filtered queries prune whole subtrees.
//   oversize - entries whose largest half-extent is above large_threshold (original +0x28,
//             default 500.0) are not put in the BVH; like the original's root links they are
//             tested by every query (a linear scan of a short list). Classified on insert, update
//             and rebuild.
//   insert  - Box2D-style greedy sibling choice by surface-area cost, then refit of ancestors.
//   remove  - sibling replaces parent, refit of ancestors.
//   update  - refit: the leaf keeps a "fat" box (bounds inflated x1.1 about the centre as the
//             original's rebuild does, plus fat_margin); a move that stays inside it touches only
//             the leaf, otherwise the fat box is reset and ancestors are refit (exact unions,
//             stopping when nothing changes). No restructuring.
//   rebuild - original split rule (TACTICAL_COMBAT.md 1.2): axis = the node box's longest extent
//             (X only if strictly longer than Y and Z, else Y if strictly longer than Z, else Z);
//             key = AABB centre on that axis; the first floor(n/2) go to child 0. The original
//             finds that count split with a 16-bin histogram (an approximate median that keeps
//             history-dependent input order inside bins); this port uses the exact median with
//             ties broken by handle (nth_element on a strict total order), so the tree is a pure
//             function of the entry set {handle, box, mask}: independent of insertion history,
//             standard library and worker count. Node indices are pre-assigned in DFS order (a
//             subtree of m leaves owns 2m-1 nodes), so the parallel path builds disjoint subtrees
//             concurrently and writes the same bytes.
//   maintain(game_time) - the original cadence: rebuild when dirty, at most once per
//             rebuild_interval (1 s) of game time; dirty when count < moved_ratio (5) x moved,
//             i.e. more than 20% of the entries were refit out of their fat box / churned.
//
// Result ORDER (deterministic, independent of tree shape and history):
//   query_aabb / query_sphere / query_circle_xy / query_frustum / query_visible: ascending handle.
//   query_ray: ascending (t_enter, handle). raycast_first: minimum (t_enter, handle).
// Results are exactly the brute-force results of the predicates in geometry.hpp. Callers apply
// caps (Max_Victims, the radius query's maximum count) AFTER this sort, and the radius query's
// exact object tests (XY distance, sphere vs oriented box) on the returned candidates.
//
// INTENTIONAL DEVIATIONS from the original TreeCull (TACTICAL_COMBAT.md section 1), none of which
// change which entries a query returns, only their order and the cost:
//   * Original order is pre-order with a node's own links first, then reversed by the intrusive
//     list; here results are sorted by object id (the doc's port rule, 0.1 / 1.6).
//   * Leaves hold one entry (original: split stops below 4 links or past depth 16); no depth cap.
//   * Between rebuilds the original never refits: links migrate up to the first containing
//     ancestor. Here ancestors are refit (HOTSPOTS 3 recommendation); the 20% dirty rule is kept,
//     counting refits that leave the fat box.
//   * Exact median instead of the 16-bin approximate median; the "axis from the parent" quirk is
//     dropped (the doc marks it harmless).
//   * Queries are reentrant (per-query QueryContext), so they can run in parallel.
//
// Filtering: every query takes a QueryFilter {include, exclude}; an entry matches when
// (mask & include) != 0 and (mask & exclude) == 0. Subtrees are pruned on include (node masks are
// ORs). Dead objects: per OBJECT_RUNTIME.md 5.3 a dying object stays linked until the
// deferred-delete flush at the end of the store update, so S1 flags it with set_dead() (the
// reserved kDeadCategory bit) and queries that must skip it pass kSkipDead or exclude that bit.
//
// Store-update integration (OBJECT_RUNTIME.md 5.3 step 1, "refit the spatial trees" at the START
// of the store update, before objects are serviced): S1 calls update() for objects whose bounds
// changed, then maintain(game_time); remove() is called from the deferred-delete flush.
//
// Allocation: queries never allocate once the caller-owned QueryContext has grown (the tests
// count operator new). update() never allocates; rebuild() reuses member scratch.
// Thread safety: const queries may run concurrently with distinct QueryContexts; mutation is
// single-threaded.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/tactical/geometry.hpp"

namespace alamo::tactical {

using EntityHandle = uint32_t;  // the gameplay object id (see header comment)
inline constexpr EntityHandle kNoEntity = 0xFFFFFFFFu;  // reserved: cannot be inserted
inline constexpr uint32_t kAllCategories = 0xFFFFFFFFu;
// Reserved category bit: set by set_dead() on objects that died this frame but are not yet unlinked.
inline constexpr uint32_t kDeadCategory = 0x80000000u;

// An entry matches when (mask & include) != 0 && (mask & exclude) == 0. Converts implicitly from a
// plain include mask, so query_sphere(s, 0x3u, ctx) works.
struct QueryFilter {
    uint32_t include = kAllCategories;
    uint32_t exclude = 0;
    constexpr QueryFilter(uint32_t inc = kAllCategories, uint32_t exc = 0) noexcept : include(inc), exclude(exc) {}
    constexpr bool matches(uint32_t mask) const noexcept { return (mask & include) != 0 && (mask & exclude) == 0; }
};
inline constexpr QueryFilter kSkipDead{kAllCategories, kDeadCategory};

struct RayHit {
    EntityHandle handle = kNoEntity;
    float t = 0.0f;
};

// Caller-owned reusable scratch + result storage. Keep one per thread / per query site; after the
// first few queries its vectors stop growing and queries are allocation-free.
struct QueryContext {
    struct StackItem {
        int32_t node;
        uint32_t flags;
    };
    std::vector<StackItem> stack;
    std::vector<EntityHandle> results;
    std::vector<RayHit> hits;
    void reserve(size_t entries) {
        stack.reserve(128);
        results.reserve(entries);
        hits.reserve(entries);
    }
};

struct SpatialTreeConfig {
    float fat_margin = 0.0f;           // extra leaf box enlargement on top of fat_inflate
    float fat_inflate = 1.1f;          // fat half-extents = tight x this (original rebuild: x1.1)
    float large_threshold = 500.0f;    // largest half-extent above this: root list (original +0x28)
    double rebuild_interval = 1.0;     // min game seconds between maintain() rebuilds (original: 1 s)
    uint32_t moved_ratio = 5;          // dirty when size < moved_ratio * moved (original: > 20%)
    uint32_t parallel_threshold = 16384;  // entries below this: rebuild and batches run serially
};

enum class QueryKind : uint8_t { Aabb, Sphere, Frustum, Ray, Visible, CircleXY };

// One query of a batch. Fields used per kind: Aabb box; Sphere sphere; Frustum frustum;
// Ray ray; CircleXY circle; Visible sphere (eye + view radius), blockers, observer. `filter`
// selects targets.
struct BatchQuery {
    QueryKind kind = QueryKind::Sphere;
    QueryFilter filter;
    QueryFilter blockers{0u};
    EntityHandle observer = kNoEntity;
    Aabb box;
    Sphere sphere;
    CircleXY circle;
    Frustum frustum;
    Ray ray;
};

// Per-query outputs of query_batch (contexts are kept between calls; they only grow).
struct BatchResults {
    std::vector<QueryContext> per_query;
    std::span<const EntityHandle> handles(size_t i) const { return per_query[i].results; }
    std::span<const RayHit> hits(size_t i) const { return per_query[i].hits; }
};

class SpatialTree {
public:
    explicit SpatialTree(SpatialTreeConfig config = {});

    // ---- mutation (single thread) ----
    // false if the handle is kNoEntity / already present, or the box is not valid().
    bool insert(EntityHandle h, const Aabb& box, uint32_t mask);
    bool remove(EntityHandle h);
    bool update(EntityHandle h, const Aabb& box);  // refit (see header comment)
    bool set_mask(EntityHandle h, uint32_t mask);
    bool set_dead(EntityHandle h, bool dead = true);  // sets / clears kDeadCategory in the mask
    void clear();
    // Full rebuild (reclassifies oversize entries); engages `pf` only when size() >=
    // parallel_threshold. Same tree either way.
    void rebuild(const ParallelFor& pf = serial_for);
    // Map-load form (Rebuild_With_Threshold): sets large_threshold, then rebuilds.
    void rebuild_with_threshold(float large_threshold, const ParallelFor& pf = serial_for);
    // Original cadence: rebuild if dirty() and game_time - last rebuild >= rebuild_interval.
    // Returns true if it rebuilt.
    bool maintain(double game_time, const ParallelFor& pf = serial_for);
    bool dirty() const noexcept { return dirty_; }

    // ---- inspection ----
    uint32_t size() const noexcept { return static_cast<uint32_t>(entries_.size()); }
    bool contains(EntityHandle h) const { return map_.find(h) != map_.end(); }
    const Aabb* bounds(EntityHandle h) const;  // tight box, nullptr if absent
    uint32_t mask_of(EntityHandle h) const;    // 0 if absent
    bool is_oversize(EntityHandle h) const;    // in the root list
    uint32_t oversize_count() const noexcept { return static_cast<uint32_t>(large_.size()); }
    uint32_t node_count() const noexcept;      // live BVH nodes
    uint32_t height() const;                   // 0 for empty BVH, 1 for a single leaf
    bool validate() const;                     // structural invariants (tests)
    // Digest of the tree shape in preorder (node boxes, masks, leaf handles, then the root list;
    // no internal indices). Tests use it to prove rebuild is history- and worker-independent.
    uint64_t structure_digest() const;
    const SpatialTreeConfig& config() const noexcept { return config_; }

    // ---- queries (const; thread-safe with distinct contexts; results live in ctx) ----
    std::span<const EntityHandle> query_aabb(const Aabb& box, QueryFilter filter, QueryContext& ctx) const;
    std::span<const EntityHandle> query_sphere(const Sphere& s, QueryFilter filter, QueryContext& ctx) const;
    // 2D radius query (XY plane, any Z): the original GOM radius query's candidate shape.
    std::span<const EntityHandle> query_circle_xy(const CircleXY& c, QueryFilter filter, QueryContext& ctx) const;
    // Render culling (no gameplay caller in the original).
    std::span<const EntityHandle> query_frustum(const Frustum& f, QueryFilter filter, QueryContext& ctx) const;
    // All hits of a ray / segment, sorted by (t_enter, handle).
    std::span<const RayHit> query_ray(const Ray& r, QueryFilter filter, QueryContext& ctx) const;
    // Nearest hit by (t_enter, handle), skipping `ignore`. False if nothing is hit.
    bool raycast_first(const Ray& r, QueryFilter filter, QueryContext& ctx, RayHit& out,
                       EntityHandle ignore = kNoEntity) const;
    // Line of sight: true if any entry matching `blockers` (other than ignore_a / ignore_b)
    // touches the segment a->b.
    bool segment_blocked(Vec3 a, Vec3 b, QueryFilter blockers, QueryContext& ctx, EntityHandle ignore_a = kNoEntity,
                         EntityHandle ignore_b = kNoEntity) const;
    // Visibility / threat check: entries matching `targets` within view.radius of view.center
    // (excluding `observer`) whose box centre is not hidden from view.center by an entry matching
    // `blockers` (the observer and the target itself never block). Ascending handle.
    std::span<const EntityHandle> query_visible(const Sphere& view, QueryFilter targets, QueryFilter blockers,
                                                EntityHandle observer, QueryContext& ctx) const;

    // Many independent queries; runs through `pf` only when size() >= parallel_threshold and
    // there is more than one query. Results are identical either way.
    void query_batch(std::span<const BatchQuery> queries, BatchResults& out, const ParallelFor& pf = serial_for) const;
    void run_query(const BatchQuery& q, QueryContext& ctx) const;

    // ---- determinism (Q1 hook) ----
    // Feeds the gameplay-visible state in canonical order: u32 count, then per entry by
    // ascending handle: u32 handle, u32 mask, 6 x f32 bit patterns (min xyz, max xyz); all
    // little-endian. Tree shape is not fed (it is a cache rebuilt from this state).
    void hash_state(const std::function<void(const void* data, size_t len)>& sink) const;

    // ---- N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/tree_save.cpp) ----
    // DECISION: the tree is saved as its ENTRY SET and REBUILT on load, not saved node by node.
    // Layout: u32 version, f32 large_threshold, u32 count, per entry by ascending handle: u32 handle,
    // u32 mask, 6 x f32 (tight min xyz, max xyz). read() inserts the entries and rebuild()s, so the
    // loaded tree is the rebuild of the saved entry set (shape, fat boxes, root-list order and node
    // indices may differ from the saving tree's). That is exact for game logic because
    //   * hash_state() feeds the entry set only (no shape);
    //   * every query returns exactly the brute-force result of its predicate on the TIGHT boxes,
    //     sorted by handle (query_ray: by (t, handle), raycast_first: minimum (t, handle);
    //     segment_blocked: a bool), and internal / fat boxes only prune conservatively - the
    //     contents and order never depend on the shape;
    //   * the maintain() cadence (moved count, dirty flag, last rebuild time) only decides when a
    //     later rebuild reshapes the tree; it is not saved (after a load the tree counts as freshly
    //     rebuilt, never by maintain(): the next dirty maintain() rebuilds at once).
    // engine/tactical/tests/test_tree_save.cpp proves it: a tree with an insert / update / remove /
    // mask / maintain history and its loaded copy give identical hash_state() and identical results
    // for every query kind, also while both keep being mutated afterwards. Existing hash
    // definitions are unchanged. read() returns false and leaves the tree unchanged on a version
    // mismatch, unsorted / duplicate / reserved handles, an invalid box or truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Node {
        Aabb box;              // internal: union of children's fat boxes; leaf: the tight box
        int32_t parent = -1;
        int32_t left = -1;     // leaf: -1
        int32_t right = -1;    // leaf: -1
        uint32_t mask = 0;     // OR of the subtree's category masks
        EntityHandle handle = kNoEntity;  // leaf only
        int32_t entry = -1;    // leaf: index into entries_; -1 for internal and free nodes
        bool leaf() const { return left < 0; }
    };
    struct Entry {
        Aabb tight;
        Aabb fat;
        EntityHandle handle;
        uint32_t mask;
        int32_t leaf;   // BVH leaf node, or -1 when in the root list
        int32_t large;  // index into large_, or -1
    };
    struct BuildItem {
        float c[3];  // min + max per axis (2 x centre; no rounding of the 0.5)
        EntityHandle handle;
        uint32_t entry;
        Aabb tight;
        uint32_t mask;
    };
    struct BuildTask {
        uint32_t begin, end;
        int32_t base, parent;
    };

    bool oversize(const Aabb& b) const;
    Aabb make_fat(const Aabb& b) const;
    void attach(uint32_t idx);  // into the BVH or the root list, by size
    void detach(uint32_t idx);
    int32_t alloc_node();
    void free_node(int32_t n);
    const Aabb& fat_of(int32_t n) const;
    void insert_leaf(int32_t leaf);
    void remove_leaf(int32_t leaf);
    void refit_up(int32_t n);
    void note_moved();
    void build_top(uint32_t b, uint32_t e, int32_t base, int32_t parent, uint32_t depth);
    void build_subtree(uint32_t b, uint32_t e, int32_t base, int32_t parent);
    uint32_t split(uint32_t b, uint32_t e);
    void finish_internal(int32_t n);

    template <class NodeFn>
    void walk(QueryContext& ctx, uint32_t root_flags, NodeFn&& fn) const;
    template <class Pred>
    void collect(const Pred& pred, QueryFilter filter, QueryContext& ctx) const;

    SpatialTreeConfig config_;
    std::vector<Node> nodes_;
    std::vector<int32_t> free_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> large_;  // entry indices of the root list (sorted by handle after rebuild)
    std::unordered_map<EntityHandle, uint32_t> map_;  // lookup only, never iterated
    int32_t root_ = -1;
    uint32_t moved_ = 0;
    bool dirty_ = false;
    bool ever_rebuilt_ = false;
    double last_rebuild_ = 0.0;
    // rebuild scratch (capacity kept between rebuilds)
    std::vector<BuildItem> items_;
    std::vector<BuildTask> tasks_;
    std::vector<int32_t> top_nodes_;
};

}  // namespace alamo::tactical
