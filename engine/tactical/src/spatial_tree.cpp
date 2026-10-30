// S6 spatial tree - see spatial_tree.hpp for the design, ordering and determinism notes.
#include "alamo/tactical/spatial_tree.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace alamo::tactical {

namespace {

// Rebuild decomposition: the top kTopDepth levels (or ranges down to kMinTask leaves) are split
// on the calling thread, the remaining subtrees are independent tasks. Depends only on the entry
// count, never on the worker count.
constexpr uint32_t kTopDepth = 6;
constexpr uint32_t kMinTask = 512;
constexpr uint32_t kBatchGrain = 16;

void put_u32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
}

bool hit_less(const RayHit& a, const RayHit& b) { return a.t < b.t || (a.t == b.t && a.handle < b.handle); }

}  // namespace

SpatialTree::SpatialTree(SpatialTreeConfig config) : config_(config) {
    if (!(config_.fat_margin >= 0.0f) || !is_finite(config_.fat_margin)) config_.fat_margin = 0.0f;
    if (!(config_.fat_inflate >= 1.0f) || !is_finite(config_.fat_inflate)) config_.fat_inflate = 1.0f;
    if (!(config_.large_threshold >= 0.0f)) config_.large_threshold = 500.0f;  // NaN / negative
    if (config_.moved_ratio == 0) config_.moved_ratio = 1;
}

// ---------------------------------------------------------------------------------------------
// classification, fat boxes

bool SpatialTree::oversize(const Aabb& b) const {
    const float t = config_.large_threshold;
    return (b.max.x - b.min.x) * 0.5f > t || (b.max.y - b.min.y) * 0.5f > t || (b.max.z - b.min.z) * 0.5f > t;
}

Aabb SpatialTree::make_fat(const Aabb& b) const {
    const float k = config_.fat_inflate;
    const Vec3 c = center(b);
    const Vec3 h{(b.max.x - b.min.x) * 0.5f * k, (b.max.y - b.min.y) * 0.5f * k, (b.max.z - b.min.z) * 0.5f * k};
    // merge with b: guarantees containment whatever the rounding of c +- h
    return expand(merge(b, Aabb{c - h, c + h}), config_.fat_margin);
}

// ---------------------------------------------------------------------------------------------
// node pool

int32_t SpatialTree::alloc_node() {
    if (!free_.empty()) {
        const int32_t n = free_.back();
        free_.pop_back();
        nodes_[static_cast<size_t>(n)] = Node{};
        return n;
    }
    nodes_.emplace_back();
    // OI tactical: the free list can never hold more than nodes_.size() entries, so sizing its capacity with the
    // node pool here (insert / rebuild time) keeps free_node() - and with it remove() - allocation-free.
    if (free_.capacity() < nodes_.capacity()) free_.reserve(nodes_.capacity());
    return static_cast<int32_t>(nodes_.size() - 1);
}

void SpatialTree::free_node(int32_t n) {
    nodes_[static_cast<size_t>(n)] = Node{};
    free_.push_back(n);
}

const Aabb& SpatialTree::fat_of(int32_t n) const {
    const Node& nd = nodes_[static_cast<size_t>(n)];
    return nd.leaf() ? entries_[static_cast<size_t>(nd.entry)].fat : nd.box;
}

void SpatialTree::refit_up(int32_t n) {
    while (n >= 0) {
        Node& nd = nodes_[static_cast<size_t>(n)];
        const Aabb b = merge(fat_of(nd.left), fat_of(nd.right));
        const uint32_t m = nodes_[static_cast<size_t>(nd.left)].mask | nodes_[static_cast<size_t>(nd.right)].mask;
        if (b == nd.box && m == nd.mask) break;  // ancestors already contain it
        nd.box = b;
        nd.mask = m;
        n = nd.parent;
    }
}

void SpatialTree::note_moved() {
    ++moved_;
    if (static_cast<uint64_t>(entries_.size()) < static_cast<uint64_t>(config_.moved_ratio) * moved_) dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// insert / remove / update

void SpatialTree::insert_leaf(int32_t leaf) {
    const Aabb fat = fat_of(leaf);
    if (root_ < 0) {
        root_ = leaf;
        nodes_[static_cast<size_t>(leaf)].parent = -1;
        return;
    }
    // Greedy descent by surface-area cost (Box2D v2.3 style); ties go left.
    int32_t idx = root_;
    while (!nodes_[static_cast<size_t>(idx)].leaf()) {
        const Node& n = nodes_[static_cast<size_t>(idx)];
        const float area = half_area(n.box);
        const float combined = half_area(merge(n.box, fat));
        const float cost = 2.0f * combined;
        const float inherit = 2.0f * (combined - area);
        auto child_cost = [&](int32_t c) {
            const Aabb& cb = fat_of(c);
            const float a = half_area(merge(fat, cb));
            return nodes_[static_cast<size_t>(c)].leaf() ? a + inherit : (a - half_area(cb)) + inherit;
        };
        const float c1 = child_cost(n.left);
        const float c2 = child_cost(n.right);
        if (cost < c1 && cost < c2) break;
        idx = c1 <= c2 ? n.left : n.right;
    }
    const int32_t sibling = idx;
    const int32_t old_parent = nodes_[static_cast<size_t>(sibling)].parent;
    const int32_t np = alloc_node();  // may reallocate nodes_
    {
        Node& p = nodes_[static_cast<size_t>(np)];
        p.parent = old_parent;
        p.left = sibling;
        p.right = leaf;
        p.box = merge(fat_of(sibling), fat);
        p.mask = nodes_[static_cast<size_t>(sibling)].mask | nodes_[static_cast<size_t>(leaf)].mask;
    }
    nodes_[static_cast<size_t>(sibling)].parent = np;
    nodes_[static_cast<size_t>(leaf)].parent = np;
    if (old_parent < 0) {
        root_ = np;
    } else {
        Node& op = nodes_[static_cast<size_t>(old_parent)];
        if (op.left == sibling)
            op.left = np;
        else
            op.right = np;
        refit_up(old_parent);
    }
}

void SpatialTree::remove_leaf(int32_t leaf) {
    if (leaf == root_) {
        root_ = -1;
        return;
    }
    const int32_t p = nodes_[static_cast<size_t>(leaf)].parent;
    const int32_t g = nodes_[static_cast<size_t>(p)].parent;
    const int32_t s = nodes_[static_cast<size_t>(p)].left == leaf ? nodes_[static_cast<size_t>(p)].right
                                                                   : nodes_[static_cast<size_t>(p)].left;
    nodes_[static_cast<size_t>(s)].parent = g;
    free_node(p);
    if (g < 0) {
        root_ = s;
    } else {
        Node& gn = nodes_[static_cast<size_t>(g)];
        if (gn.left == p)
            gn.left = s;
        else
            gn.right = s;
        refit_up(g);
    }
}

void SpatialTree::attach(uint32_t idx) {
    Entry& e = entries_[idx];
    e.fat = make_fat(e.tight);
    if (oversize(e.tight)) {
        e.leaf = -1;
        e.large = static_cast<int32_t>(large_.size());
        large_.push_back(idx);
        return;
    }
    e.large = -1;
    const int32_t leaf = alloc_node();
    Node& l = nodes_[static_cast<size_t>(leaf)];
    l.box = e.tight;
    l.mask = e.mask;
    l.handle = e.handle;
    l.entry = static_cast<int32_t>(idx);
    entries_[idx].leaf = leaf;
    insert_leaf(leaf);
}

void SpatialTree::detach(uint32_t idx) {
    Entry& e = entries_[idx];
    if (e.leaf >= 0) {
        remove_leaf(e.leaf);
        free_node(e.leaf);
        e.leaf = -1;
        return;
    }
    const auto slot = static_cast<size_t>(e.large);
    const uint32_t last = large_.back();
    large_[slot] = last;
    entries_[last].large = static_cast<int32_t>(slot);
    large_.pop_back();
    entries_[idx].large = -1;
}

bool SpatialTree::insert(EntityHandle h, const Aabb& box, uint32_t mask) {
    if (h == kNoEntity || !valid(box) || map_.find(h) != map_.end()) return false;
    const auto idx = static_cast<uint32_t>(entries_.size());
    entries_.push_back(Entry{box, box, h, mask, -1, -1});
    map_.emplace(h, idx);
    attach(idx);
    note_moved();
    return true;
}

bool SpatialTree::remove(EntityHandle h) {
    const auto it = map_.find(h);
    if (it == map_.end()) return false;
    const uint32_t idx = it->second;
    map_.erase(it);
    detach(idx);
    const uint32_t last = static_cast<uint32_t>(entries_.size() - 1);
    if (idx != last) {
        entries_[idx] = entries_[last];
        const Entry& m = entries_[idx];
        if (m.leaf >= 0)
            nodes_[static_cast<size_t>(m.leaf)].entry = static_cast<int32_t>(idx);
        else
            large_[static_cast<size_t>(m.large)] = idx;
        map_[m.handle] = idx;
    }
    entries_.pop_back();
    note_moved();
    return true;
}

bool SpatialTree::update(EntityHandle h, const Aabb& box) {
    if (!valid(box)) return false;
    const auto it = map_.find(h);
    if (it == map_.end()) return false;
    const uint32_t idx = it->second;
    Entry& e = entries_[idx];
    e.tight = box;
    const bool was_large = e.leaf < 0;
    if (oversize(box) != was_large) {  // size class changed: move between BVH and root list
        detach(idx);
        attach(idx);
        note_moved();
        return true;
    }
    if (was_large) {
        if (!tactical::contains(e.fat, box)) e.fat = make_fat(box);  // unused by queries; kept consistent
        return true;
    }
    nodes_[static_cast<size_t>(e.leaf)].box = box;
    if (tactical::contains(e.fat, box)) return true;  // ancestors still contain the fat box
    e.fat = make_fat(box);
    refit_up(nodes_[static_cast<size_t>(e.leaf)].parent);
    note_moved();
    return true;
}

bool SpatialTree::set_dead(EntityHandle h, bool dead) {
    const auto it = map_.find(h);
    if (it == map_.end()) return false;
    const uint32_t m = entries_[it->second].mask;
    return set_mask(h, dead ? (m | kDeadCategory) : (m & ~kDeadCategory));
}

bool SpatialTree::set_mask(EntityHandle h, uint32_t mask) {
    const auto it = map_.find(h);
    if (it == map_.end()) return false;
    Entry& e = entries_[it->second];
    e.mask = mask;
    if (e.leaf < 0) return true;
    nodes_[static_cast<size_t>(e.leaf)].mask = mask;
    int32_t n = nodes_[static_cast<size_t>(e.leaf)].parent;  // propagate while ORs change
    while (n >= 0) {
        Node& nd = nodes_[static_cast<size_t>(n)];
        const uint32_t m = nodes_[static_cast<size_t>(nd.left)].mask | nodes_[static_cast<size_t>(nd.right)].mask;
        if (m == nd.mask) break;
        nd.mask = m;
        n = nd.parent;
    }
    return true;
}

void SpatialTree::clear() {
    nodes_.clear();
    free_.clear();
    entries_.clear();
    large_.clear();
    map_.clear();
    root_ = -1;
    moved_ = 0;
    dirty_ = false;
}

const Aabb* SpatialTree::bounds(EntityHandle h) const {
    const auto it = map_.find(h);
    return it == map_.end() ? nullptr : &entries_[it->second].tight;
}

uint32_t SpatialTree::mask_of(EntityHandle h) const {
    const auto it = map_.find(h);
    return it == map_.end() ? 0u : entries_[it->second].mask;
}

bool SpatialTree::is_oversize(EntityHandle h) const {
    const auto it = map_.find(h);
    return it != map_.end() && entries_[it->second].leaf < 0;
}

uint32_t SpatialTree::node_count() const noexcept { return static_cast<uint32_t>(nodes_.size() - free_.size()); }

// ---------------------------------------------------------------------------------------------
// rebuild

uint32_t SpatialTree::split(uint32_t b, uint32_t e) {
    // node box = union of the range's tight boxes
    Aabb nb = items_[b].tight;
    for (uint32_t i = b + 1; i < e; ++i) nb = merge(nb, items_[i].tight);
    const float ex = nb.max.x - nb.min.x, ey = nb.max.y - nb.min.y, ez = nb.max.z - nb.min.z;
    // TACTICAL_COMBAT.md 1.2: X only when strictly longer than both, else Y when strictly
    // longer than Z, else Z.
    const int axis = (ex > ey && ex > ez) ? 0 : (ey > ez ? 1 : 2);
    const uint32_t mid = b + (e - b) / 2;  // first floor(n/2) go to child 0
    // Strict total order (centre, handle) -> the set of the (mid-b) smallest items is unique, so
    // the subtree contents do not depend on the nth_element implementation or the input order.
    std::nth_element(items_.begin() + b, items_.begin() + mid, items_.begin() + e,
                     [axis](const BuildItem& x, const BuildItem& y) {
                         return x.c[axis] < y.c[axis] || (x.c[axis] == y.c[axis] && x.handle < y.handle);
                     });
    return mid;
}

void SpatialTree::finish_internal(int32_t n) {
    Node& nd = nodes_[static_cast<size_t>(n)];
    nd.box = merge(fat_of(nd.left), fat_of(nd.right));
    nd.mask = nodes_[static_cast<size_t>(nd.left)].mask | nodes_[static_cast<size_t>(nd.right)].mask;
}

void SpatialTree::build_subtree(uint32_t b, uint32_t e, int32_t base, int32_t parent) {
    Node& nd = nodes_[static_cast<size_t>(base)];
    nd = Node{};
    nd.parent = parent;
    if (e - b == 1) {
        const BuildItem& it = items_[b];
        nd.box = it.tight;
        nd.mask = it.mask;
        nd.handle = it.handle;
        nd.entry = static_cast<int32_t>(it.entry);
        Entry& en = entries_[it.entry];
        en.fat = make_fat(it.tight);
        en.leaf = base;
        en.large = -1;
        return;
    }
    const uint32_t mid = split(b, e);
    nd.left = base + 1;
    nd.right = base + static_cast<int32_t>(2 * (mid - b));
    build_subtree(b, mid, nd.left, base);
    build_subtree(mid, e, nd.right, base);
    finish_internal(base);
}

void SpatialTree::build_top(uint32_t b, uint32_t e, int32_t base, int32_t parent, uint32_t depth) {
    if (e - b <= kMinTask || depth == kTopDepth) {
        tasks_.push_back(BuildTask{b, e, base, parent});
        return;
    }
    const uint32_t mid = split(b, e);
    Node& nd = nodes_[static_cast<size_t>(base)];
    nd = Node{};
    nd.parent = parent;
    nd.left = base + 1;
    nd.right = base + static_cast<int32_t>(2 * (mid - b));
    top_nodes_.push_back(base);
    const int32_t right = nd.right;
    build_top(b, mid, base + 1, base, depth + 1);
    build_top(mid, e, right, base, depth + 1);
}

void SpatialTree::rebuild(const ParallelFor& pf) {
    const auto n = static_cast<uint32_t>(entries_.size());
    items_.clear();
    large_.clear();
    for (uint32_t i = 0; i < n; ++i) {
        Entry& en = entries_[i];
        const Aabb& t = en.tight;
        if (oversize(t)) {  // root list (reclassified with the current threshold)
            en.leaf = -1;
            en.fat = make_fat(t);
            large_.push_back(i);
            continue;
        }
        items_.push_back(BuildItem{{t.min.x + t.max.x, t.min.y + t.max.y, t.min.z + t.max.z}, en.handle, i, t, en.mask});
    }
    std::sort(large_.begin(), large_.end(),
              [this](uint32_t x, uint32_t y) { return entries_[x].handle < entries_[y].handle; });
    for (size_t k = 0; k < large_.size(); ++k) entries_[large_[k]].large = static_cast<int32_t>(k);
    nodes_.clear();
    free_.clear();
    tasks_.clear();
    top_nodes_.clear();
    root_ = -1;
    moved_ = 0;
    dirty_ = false;
    const auto m = static_cast<uint32_t>(items_.size());
    if (m == 0) return;
    nodes_.resize(static_cast<size_t>(2) * m - 1);
    if (free_.capacity() < nodes_.capacity()) free_.reserve(nodes_.capacity());  // OI tactical: see alloc_node()
    build_top(0, m, 0, -1, 0);
    const auto task_count = static_cast<uint32_t>(tasks_.size());
    if (task_count > 1 && n >= config_.parallel_threshold) {
        SpatialTree* self = this;
        pf(task_count, 1, [self](uint32_t tb, uint32_t te) {
            for (uint32_t t = tb; t < te; ++t) {
                const BuildTask& k = self->tasks_[t];
                self->build_subtree(k.begin, k.end, k.base, k.parent);
            }
        });
    } else {
        for (const BuildTask& k : tasks_) build_subtree(k.begin, k.end, k.base, k.parent);
    }
    for (auto it = top_nodes_.rbegin(); it != top_nodes_.rend(); ++it) finish_internal(*it);  // children first
    root_ = 0;
}

void SpatialTree::rebuild_with_threshold(float large_threshold, const ParallelFor& pf) {
    if (large_threshold >= 0.0f) config_.large_threshold = large_threshold;
    rebuild(pf);
}

bool SpatialTree::maintain(double game_time, const ParallelFor& pf) {
    if (!dirty_) return false;
    if (ever_rebuilt_ && game_time - last_rebuild_ < config_.rebuild_interval) return false;
    rebuild(pf);
    last_rebuild_ = game_time;
    ever_rebuilt_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// diagnostics

uint32_t SpatialTree::height() const {
    if (root_ < 0) return 0;
    std::vector<std::pair<int32_t, uint32_t>> st{{root_, 1u}};
    uint32_t h = 0;
    while (!st.empty()) {
        const auto [n, d] = st.back();
        st.pop_back();
        h = std::max(h, d);
        const Node& nd = nodes_[static_cast<size_t>(n)];
        if (!nd.leaf()) {
            st.emplace_back(nd.left, d + 1);
            st.emplace_back(nd.right, d + 1);
        }
    }
    return h;
}

bool SpatialTree::validate() const {
    if (map_.size() != entries_.size()) return false;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        const Entry& en = entries_[i];
        const auto it = map_.find(en.handle);
        if (it == map_.end() || it->second != i) return false;
        if (!tactical::contains(en.fat, en.tight)) return false;
        if ((en.leaf < 0) == (en.large < 0)) return false;  // exactly one home
        if (en.large >= 0 && (static_cast<size_t>(en.large) >= large_.size() || large_[static_cast<size_t>(en.large)] != i))
            return false;
    }
    if (root_ < 0) return entries_.size() == large_.size() && node_count() == 0;
    if (nodes_[static_cast<size_t>(root_)].parent != -1) return false;
    std::vector<int32_t> st{root_};
    uint32_t visited = 0, leaves = 0;
    while (!st.empty()) {
        const int32_t n = st.back();
        st.pop_back();
        ++visited;
        const Node& nd = nodes_[static_cast<size_t>(n)];
        if (nd.leaf()) {
            ++leaves;
            if (nd.entry < 0 || static_cast<size_t>(nd.entry) >= entries_.size()) return false;
            const Entry& en = entries_[static_cast<size_t>(nd.entry)];
            if (en.leaf != n || en.handle != nd.handle || en.mask != nd.mask || !(en.tight == nd.box)) return false;
            if (nd.right >= 0) return false;
            continue;
        }
        if (nd.right < 0) return false;
        const Node& l = nodes_[static_cast<size_t>(nd.left)];
        const Node& r = nodes_[static_cast<size_t>(nd.right)];
        if (l.parent != n || r.parent != n) return false;
        if (!tactical::contains(nd.box, fat_of(nd.left)) || !tactical::contains(nd.box, fat_of(nd.right))) return false;
        if (nd.mask != (l.mask | r.mask)) return false;
        st.push_back(nd.left);
        st.push_back(nd.right);
    }
    return leaves + large_.size() == entries_.size() && visited == node_count();
}

uint64_t SpatialTree::structure_digest() const {
    uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a over 32-bit words
    auto mix = [&h](uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xFFu;
            h *= 0x100000001b3ull;
        }
    };
    auto mix_box = [&mix](const Aabb& b) {
        const float f[6] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        for (float v : f) mix(std::bit_cast<uint32_t>(v));
    };
    if (root_ >= 0) {
        std::vector<int32_t> st{root_};
        while (!st.empty()) {
            const int32_t n = st.back();
            st.pop_back();
            const Node& nd = nodes_[static_cast<size_t>(n)];
            mix_box(nd.box);
            mix(nd.mask);
            mix(nd.leaf() ? nd.handle : 0xABCDEF01u);
            if (!nd.leaf()) {
                st.push_back(nd.right);
                st.push_back(nd.left);
            }
        }
    }
    mix(0x524F4F54u);  // "ROOT" list
    for (uint32_t i : large_) {
        mix(entries_[i].handle);
        mix_box(entries_[i].tight);
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// queries

template <class NodeFn>
void SpatialTree::walk(QueryContext& ctx, uint32_t root_flags, NodeFn&& fn) const {
    ctx.stack.clear();
    if (root_ < 0) return;
    ctx.stack.push_back({root_, root_flags});
    while (!ctx.stack.empty()) {
        const QueryContext::StackItem it = ctx.stack.back();
        ctx.stack.pop_back();
        const Node& nd = nodes_[static_cast<size_t>(it.node)];
        uint32_t flags = it.flags;
        if (!fn(nd, flags)) continue;  // pruned, or a leaf (fn emitted it)
        ctx.stack.push_back({nd.right, flags});
        ctx.stack.push_back({nd.left, flags});
    }
}

// Shapes with a plain box predicate (sphere, circle): walk + root list + sort.
template <class Pred>
void SpatialTree::collect(const Pred& pred, QueryFilter filter, QueryContext& ctx) const {
    ctx.results.clear();
    walk(ctx, 0u, [&](const Node& nd, uint32_t&) {
        if ((nd.mask & filter.include) == 0 || !pred(nd.box)) return false;
        if (nd.leaf()) {
            if ((nd.mask & filter.exclude) == 0) ctx.results.push_back(nd.handle);
            return false;
        }
        return true;
    });
    for (uint32_t i : large_) {
        const Entry& en = entries_[i];
        if (filter.matches(en.mask) && pred(en.tight)) ctx.results.push_back(en.handle);
    }
    std::sort(ctx.results.begin(), ctx.results.end());
}

std::span<const EntityHandle> SpatialTree::query_aabb(const Aabb& box, QueryFilter filter, QueryContext& ctx) const {
    ctx.results.clear();
    // flags bit 0: node fully inside the query box -> descendants overlap without testing
    walk(ctx, 0u, [&](const Node& nd, uint32_t& flags) {
        if ((nd.mask & filter.include) == 0) return false;
        if ((flags & 1u) == 0) {
            if (!overlaps(box, nd.box)) return false;
            if (tactical::contains(box, nd.box)) flags |= 1u;
        }
        if (nd.leaf()) {
            if ((nd.mask & filter.exclude) == 0) ctx.results.push_back(nd.handle);
            return false;
        }
        return true;
    });
    for (uint32_t i : large_) {
        const Entry& en = entries_[i];
        if (filter.matches(en.mask) && overlaps(box, en.tight)) ctx.results.push_back(en.handle);
    }
    std::sort(ctx.results.begin(), ctx.results.end());
    return ctx.results;
}

std::span<const EntityHandle> SpatialTree::query_sphere(const Sphere& s, QueryFilter filter, QueryContext& ctx) const {
    collect([&s](const Aabb& b) { return overlaps(s, b); }, filter, ctx);
    return ctx.results;
}

std::span<const EntityHandle> SpatialTree::query_circle_xy(const CircleXY& c, QueryFilter filter,
                                                           QueryContext& ctx) const {
    collect([&c](const Aabb& b) { return overlaps(c, b); }, filter, ctx);
    return ctx.results;
}

std::span<const EntityHandle> SpatialTree::query_frustum(const Frustum& f, QueryFilter filter, QueryContext& ctx) const {
    ctx.results.clear();
    // flags: planes still to test; a plane the node is fully inside is dropped for the subtree
    walk(ctx, 0x3Fu, [&](const Node& nd, uint32_t& flags) {
        if ((nd.mask & filter.include) == 0) return false;
        for (uint32_t p = 0; p < 6; ++p) {
            if ((flags & (1u << p)) == 0) continue;
            if (plane_max(f.planes[p], nd.box) < 0.0f) return false;
            if (plane_min(f.planes[p], nd.box) >= 0.0f) flags &= ~(1u << p);
        }
        if (nd.leaf()) {
            if ((nd.mask & filter.exclude) == 0) ctx.results.push_back(nd.handle);
            return false;
        }
        return true;
    });
    for (uint32_t i : large_) {
        const Entry& en = entries_[i];
        if (filter.matches(en.mask) && overlaps(f, en.tight)) ctx.results.push_back(en.handle);
    }
    std::sort(ctx.results.begin(), ctx.results.end());
    return ctx.results;
}

std::span<const RayHit> SpatialTree::query_ray(const Ray& r, QueryFilter filter, QueryContext& ctx) const {
    ctx.hits.clear();
    const PreparedRay pr(r);
    walk(ctx, 0u, [&](const Node& nd, uint32_t&) {
        float t = 0.0f;
        if ((nd.mask & filter.include) == 0 || !ray_aabb(pr, nd.box, t)) return false;
        if (nd.leaf()) {
            if ((nd.mask & filter.exclude) == 0) ctx.hits.push_back(RayHit{nd.handle, t});
            return false;
        }
        return true;
    });
    for (uint32_t i : large_) {
        const Entry& en = entries_[i];
        float t = 0.0f;
        if (filter.matches(en.mask) && ray_aabb(pr, en.tight, t)) ctx.hits.push_back(RayHit{en.handle, t});
    }
    std::sort(ctx.hits.begin(), ctx.hits.end(), hit_less);
    return ctx.hits;
}

bool SpatialTree::raycast_first(const Ray& r, QueryFilter filter, QueryContext& ctx, RayHit& out,
                                EntityHandle ignore) const {
    const PreparedRay pr(r);
    bool found = false;
    RayHit best;
    auto offer = [&](EntityHandle h, float t) {
        if (h != ignore && (!found || hit_less(RayHit{h, t}, best))) {
            best = RayHit{h, t};
            found = true;
        }
    };
    for (uint32_t i : large_) {  // root list first: a near oversize hit prunes the walk
        const Entry& en = entries_[i];
        float t = 0.0f;
        if (filter.matches(en.mask) && ray_aabb(pr, en.tight, t)) offer(en.handle, t);
    }
    walk(ctx, 0u, [&](const Node& nd, uint32_t&) {
        float t = 0.0f;
        if ((nd.mask & filter.include) == 0 || !ray_aabb(pr, nd.box, t)) return false;
        if (found && t > best.t) return false;  // equal t may still win on handle
        if (nd.leaf()) {
            if ((nd.mask & filter.exclude) == 0) offer(nd.handle, t);
            return false;
        }
        return true;
    });
    if (found) out = best;
    return found;
}

bool SpatialTree::segment_blocked(Vec3 a, Vec3 b, QueryFilter blockers, QueryContext& ctx, EntityHandle ignore_a,
                                  EntityHandle ignore_b) const {
    ctx.stack.clear();
    if (blockers.include == 0) return false;
    const PreparedRay pr(Ray{a, b - a, 1.0f});
    for (uint32_t i : large_) {
        const Entry& en = entries_[i];
        float t = 0.0f;
        if (en.handle != ignore_a && en.handle != ignore_b && blockers.matches(en.mask) && ray_aabb(pr, en.tight, t))
            return true;
    }
    if (root_ < 0) return false;
    ctx.stack.push_back({root_, 0u});
    while (!ctx.stack.empty()) {
        const int32_t n = ctx.stack.back().node;
        ctx.stack.pop_back();
        const Node& nd = nodes_[static_cast<size_t>(n)];
        float t = 0.0f;
        if ((nd.mask & blockers.include) == 0 || !ray_aabb(pr, nd.box, t)) continue;
        if (nd.leaf()) {
            if (nd.handle != ignore_a && nd.handle != ignore_b && (nd.mask & blockers.exclude) == 0) {
                ctx.stack.clear();
                return true;  // any hit: the answer is a bool, so traversal order cannot leak
            }
            continue;
        }
        ctx.stack.push_back({nd.right, 0u});
        ctx.stack.push_back({nd.left, 0u});
    }
    return false;
}

std::span<const EntityHandle> SpatialTree::query_visible(const Sphere& view, QueryFilter targets,
                                                         QueryFilter blockers, EntityHandle observer,
                                                         QueryContext& ctx) const {
    query_sphere(view, targets, ctx);
    size_t keep = 0;
    for (size_t i = 0; i < ctx.results.size(); ++i) {
        const EntityHandle h = ctx.results[i];
        if (h == observer) continue;
        const Aabb* b = bounds(h);
        if (segment_blocked(view.center, center(*b), blockers, ctx, observer, h)) continue;
        ctx.results[keep++] = h;
    }
    ctx.results.resize(keep);
    return ctx.results;
}

void SpatialTree::run_query(const BatchQuery& q, QueryContext& ctx) const {
    switch (q.kind) {
        case QueryKind::Aabb: ctx.hits.clear(); query_aabb(q.box, q.filter, ctx); break;
        case QueryKind::Sphere: ctx.hits.clear(); query_sphere(q.sphere, q.filter, ctx); break;
        case QueryKind::CircleXY: ctx.hits.clear(); query_circle_xy(q.circle, q.filter, ctx); break;
        case QueryKind::Frustum: ctx.hits.clear(); query_frustum(q.frustum, q.filter, ctx); break;
        case QueryKind::Ray: ctx.results.clear(); query_ray(q.ray, q.filter, ctx); break;
        case QueryKind::Visible:
            ctx.hits.clear();
            query_visible(q.sphere, q.filter, q.blockers, q.observer, ctx);
            break;
    }
}

void SpatialTree::query_batch(std::span<const BatchQuery> queries, BatchResults& out, const ParallelFor& pf) const {
    const size_t n = queries.size();
    if (out.per_query.size() < n) out.per_query.resize(n);  // grow only: contexts keep capacity
    if (n > 1 && size() >= config_.parallel_threshold) {
        struct Job {
            const SpatialTree* tree;
            const BatchQuery* q;
            QueryContext* ctx;
        } job{this, queries.data(), out.per_query.data()};
        const Job* jp = &job;  // one-pointer capture: fits std::function's small buffer
        pf(static_cast<uint32_t>(n), kBatchGrain, [jp](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) jp->tree->run_query(jp->q[i], jp->ctx[i]);
        });
    } else {
        for (size_t i = 0; i < n; ++i) run_query(queries[i], out.per_query[i]);
    }
}

// ---------------------------------------------------------------------------------------------
// CRC hook

void SpatialTree::hash_state(const std::function<void(const void*, size_t)>& sink) const {
    std::vector<std::pair<EntityHandle, uint32_t>> order;
    order.reserve(entries_.size());
    for (uint32_t i = 0; i < entries_.size(); ++i) order.emplace_back(entries_[i].handle, i);
    std::sort(order.begin(), order.end());
    unsigned char buf[32];
    put_u32(buf, static_cast<uint32_t>(order.size()));
    sink(buf, 4);
    for (const auto& [h, i] : order) {
        const Entry& en = entries_[i];
        const Aabb& b = en.tight;
        put_u32(buf + 0, h);
        put_u32(buf + 4, en.mask);
        const float f[6] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        for (int k = 0; k < 6; ++k) put_u32(buf + 8 + 4 * k, std::bit_cast<uint32_t>(f[k]));
        sink(buf, sizeof(buf));
    }
}

}  // namespace alamo::tactical
