#include "alamo/galactic/all_pairs.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace alamo::galactic {

namespace {
// cvttss2si / cvttsd2si with a 64-bit destination: truncation toward zero, NaN or out of range
// => 0x8000000000000000 ("integer indefinite").
constexpr double kTwo63 = 9223372036854775808.0;
int64_t trunc_to_int64(double x) {
    if (x >= -kTwo63 && x < kTwo63) return static_cast<int64_t>(x);
    return std::numeric_limits<int64_t>::min();
}
int64_t trunc_to_int64(float x) {
    if (x >= -9223372036854775808.0f && x < 9223372036854775808.0f) return static_cast<int64_t>(x);
    return std::numeric_limits<int64_t>::min();
}
}  // namespace

uint32_t node_cost_from_value(double value) {
    // Q7: trunc toward zero to int64, low 32 bits as unsigned, unsigned / 100.
    const uint64_t bits = static_cast<uint64_t>(trunc_to_int64(value));
    return static_cast<uint32_t>(bits & 0xFFFFFFFFull) / 100u;
}

uint16_t hop_from_distance(float travel) {
    // Q8 step 4: the multiply is float, so (distance / HS) * 10.0f, then truncate.
    const float x = travel * 10.0f;
    return static_cast<uint16_t>(static_cast<uint64_t>(trunc_to_int64(x)) & 0xFFFFull);
}

void AllPairsMatrices::resize(uint32_t n) {
    n_ = n;
    weight_.assign(size_t(n) * n, kUnlinked);
    pred_.assign(size_t(n) * n, kNoPred);
    node_.assign(n, 1);
}

bool AllPairsMatrices::get_path(uint32_t from, uint32_t to, std::vector<PlanetIndex>& path,
                                std::vector<uint32_t>* hop_weights) const {
    // Q10, Get_Path (0x690f10).
    path.clear();
    if (hop_weights) hop_weights->clear();
    if (!is_node(from) || !is_node(to)) return false;
    if (from == to) {
        path = {from, from};
        if (hop_weights) *hop_weights = {0u, 0u};
        return true;
    }
    // Walk back from dst, collecting at the back and reversing once (== front insertion), O(L).
    uint32_t cur = to;
    for (int32_t u = pred(from, cur); u >= 0; u = pred(from, cur)) {
        if (path.size() >= n_) {  // corrupt (cyclic) chain guard; the original would not stop
            path.clear();
            if (hop_weights) hop_weights->clear();
            return false;
        }
        const uint32_t uu = static_cast<uint32_t>(u);
        path.push_back(uu);
        if (hop_weights) hop_weights->push_back(weight(uu, cur));
        cur = uu;
    }
    if (path.empty()) return false;
    std::reverse(path.begin(), path.end());
    path.push_back(to);
    if (hop_weights) {
        std::reverse(hop_weights->begin(), hop_weights->end());
        hop_weights->push_back(0u);
    }
    return true;
}

namespace detail {

void build_raw(const PlanetGraph& graph, const AllPairsInput& input, RawOwned& o,
               std::vector<uint32_t>* cost_out, std::vector<uint8_t>* hidden_out) {
    const uint32_t n = graph.size();
    std::vector<uint32_t> cost(n, 0);
    for (uint32_t i = 0; i < n && i < input.node_value.size(); ++i) cost[i] = node_cost_from_value(input.node_value[i]);
    std::vector<uint8_t> hidden(n, 0);
    if (input.revealed && !input.revealed->empty())
        for (uint32_t i = 0; i < n; ++i) hidden[i] = (i < input.revealed->size() && input.revealed->test(i)) ? 0 : 1;

    o.n = n;
    o.offsets.assign(1, 0);
    o.nbr.clear();
    o.w.clear();
    o.node.resize(n);
    const float max_move = graph.max_fleet_move_distance();
    auto emit = [&](uint32_t v, float travel) {
        const uint32_t w = pack_edge_weight(cost[v], hop_from_distance(travel));
        if (w == kUnlinked) return;  // Q8 quirk: a linked 0xFFFFFFFF reads as "unlinked"
        o.nbr.push_back(v);
        o.w.push_back(w);
    };
    for (uint32_t i = 0; i < n; ++i) {
        o.node[i] = graph.included(i) ? 1 : 0;
        // Q8 step 2: fog (either endpoint unrevealed) => unlinked.
        if (graph.included(i) && !hidden[i]) {
            const float zadj = i < input.z_offset.size() ? input.z_offset[i] : 0.0f;
            const uint32_t eb = graph.edge_begin(i), ee = graph.edge_end(i);
            if (zadj == 0.0f) {
                // Same distance bits and the same inclusive test as the graph build, so the AI's
                // link set from i is exactly i's graph edges (routes are always graph edges).
                for (uint32_t e = eb; e < ee; ++e) {
                    const PlanetIndex v = graph.neighbour(e);
                    if (hidden[v]) continue;
                    const float d = graph.distance(e);
                    emit(v, graph.route(e) == kNoRoute ? d : d / graph.speed_factor(e));
                }
            } else {
                // Q2: source type's Layer_Z_Adjust added to the destination's z; the adjusted
                // distance drives both the free-hop test and the cost, so scan every node.
                uint32_t e = eb;
                for (uint32_t j = 0; j < n; ++j) {
                    while (e < ee && graph.neighbour(e) < j) ++e;
                    if (j == i || !graph.included(j) || hidden[j]) continue;
                    Vec3 pj = graph.position(j);
                    pj.z = pj.z + zadj;
                    const float d = planet_distance(graph.position(i), pj);
                    const bool has_route = e < ee && graph.neighbour(e) == j && graph.route(e) != kNoRoute;
                    if (!has_route && d > max_move) continue;  // Q8 step 3
                    emit(j, has_route ? d / graph.speed_factor(e) : d);
                }
            }
        }
        o.offsets.push_back(static_cast<uint32_t>(o.nbr.size()));
    }
    if (cost_out) *cost_out = std::move(cost);
    if (hidden_out) *hidden_out = std::move(hidden);
}

uint64_t walk_bound(const RawGraph& g) {
    std::vector<uint32_t> maxin(g.n, 0);
    for (uint32_t a = 0; a < g.n; ++a)
        for (uint32_t e = g.offsets[a]; e < g.offsets[a + 1]; ++e) maxin[g.nbr[e]] = std::max(maxin[g.nbr[e]], g.w[e]);
    uint64_t s = 0;
    for (uint32_t v : maxin) s += v;
    return 2 * s;
}

// ---- Reference: the original's Floyd-Warshall (Q9) -----------------------------------------
//  * diagonal weight 0 / pred -1; off-diagonal weight = Edge_Weight, pred = row index or -1.
//  * k outer, i, j: skip when W[i][k] or W[k][j] is 0xFFFFFFFF; sum in uint32 (wrapping);
//    relax when sum < W[i][j] unsigned (so "unlinked" = UINT32_MAX loses to any other sum);
//    pred[i][j] = pred[k][j].
//  * Row k and column k never change in phase k, even with wrapping: the diagonal stays 0 (no
//    unsigned sum is < 0), so a sum through k itself equals the current value and never
//    relaxes. Rows are therefore independent for a fixed k and the i loop may run in parallel;
//    reading W[i][k] once per row equals re-reading it for each j.
void reference_raw(const RawGraph& g, AllPairsMatrices& out, const ParallelFor& pf) {
    const uint32_t n = g.n;
    out.resize(n);
    if (!g.node.empty()) out.mutable_nodes().assign(g.node.begin(), g.node.end());
    uint32_t* W = out.mutable_weights().data();
    int32_t* P = out.mutable_preds().data();
    for (uint32_t i = 0; i < n; ++i) {
        W[size_t(i) * n + i] = 0;
        P[size_t(i) * n + i] = kNoPred;
        for (uint32_t e = g.offsets[i]; e < g.offsets[i + 1]; ++e) {
            W[size_t(i) * n + g.nbr[e]] = g.w[e];
            P[size_t(i) * n + g.nbr[e]] = static_cast<int32_t>(i);
        }
    }
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t* Wk = W + size_t(k) * n;
        const int32_t* Pk = P + size_t(k) * n;
        pf(n, 16, [&](uint32_t ib, uint32_t ie) {
            for (uint32_t i = ib; i < ie; ++i) {
                uint32_t* Wi = W + size_t(i) * n;
                const uint32_t wik = Wi[k];
                if (wik == kUnlinked) continue;
                int32_t* Pi = P + size_t(i) * n;
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t wkj = Wk[j];
                    if (wkj == kUnlinked) continue;
                    const uint32_t sum = wik + wkj;  // wraps
                    if (sum < Wi[j]) {
                        Wi[j] = sum;
                        Pi[j] = Pk[j];
                    }
                }
            }
        });
    }
}

// ---- Fast: per-target reverse Dijkstra + bottleneck derivation of FW's predecessor ----------
namespace {

constexpr uint64_t kInf = std::numeric_limits<uint64_t>::max();
constexpr int32_t kUnset = std::numeric_limits<int32_t>::max();
constexpr int32_t kUnknown = -2;

// Monotone integer priority queue (radix heap): valid for Dijkstra because popped keys never
// decrease. Order among equal keys is irrelevant: only exact distances are taken from it.
class RadixHeap {
public:
    void clear() {
        for (auto& b : buckets_) b.clear();
        mask_ = 0;
        last_ = 0;
        size_ = 0;
    }
    bool empty() const { return size_ == 0; }
    void push(uint64_t key, uint32_t v) {
        put(key, v);
        ++size_;
    }
    std::pair<uint64_t, uint32_t> pop() {
        if (buckets_[0].empty()) {
            const size_t i = size_t(std::countr_zero(mask_)) + 1;  // lowest non-empty bucket 1..64
            auto& src = buckets_[i];
            uint64_t mn = src[0].first;
            for (const auto& x : src) mn = std::min(mn, x.first);
            last_ = mn;
            mask_ &= ~(1ull << (i - 1));
            for (const auto& x : src) put(x.first, x.second);  // all land in buckets < i
            src.clear();
        }
        const auto r = buckets_[0].back();
        buckets_[0].pop_back();
        --size_;
        return r;
    }

private:
    void put(uint64_t key, uint32_t v) {
        const size_t b = key == last_ ? 0 : size_t(64 - std::countl_zero(key ^ last_));
        buckets_[b].emplace_back(key, v);
        if (b) mask_ |= 1ull << (b - 1);
    }
    std::vector<std::pair<uint64_t, uint32_t>> buckets_[65];
    uint64_t mask_ = 0;
    uint64_t last_ = 0;
    size_t size_ = 0;
};

struct InEdges {  // reverse CSR: in-edges a -> v of each v
    std::vector<uint32_t> offsets, src, w;
};

struct ColumnScratch {
    std::vector<uint64_t> dist;
    std::vector<int32_t> m;  // M(a, j): min-max intermediate over shortest a->j walks
    std::vector<int32_t> pred;
    RadixHeap heap;
    std::vector<uint32_t> stack;
    std::vector<uint32_t> chain;
};

// Column j: writes W[a][j] / P[a][j] for every a (disjoint per-index outputs per column).
void solve_column(const InEdges& in, uint32_t n, uint32_t j, ColumnScratch& s, uint32_t* W, int32_t* P) {
    s.dist.assign(n, kInf);
    s.dist[j] = 0;
    // 1) Reverse Dijkstra over in-edges: dist[a] = D(a, j), exact (eligibility => < 2^32).
    auto& h = s.heap;
    h.clear();
    h.push(0, j);
    while (!h.empty()) {
        const auto [d, v] = h.pop();
        if (d != s.dist[v]) continue;
        for (uint32_t e = in.offsets[v]; e < in.offsets[v + 1]; ++e) {
            const uint32_t a = in.src[e];
            const uint64_t nd = d + in.w[e];
            if (nd < s.dist[a]) {
                s.dist[a] = nd;
                h.push(nd, a);
            }
        }
    }
    // 2) M(a) = min over tight edges a->v of (v == j ? -1 : max(v, M(v))), a->v tight when
    //    D(a) == w(a,v) + D(v). Threshold sweep k = -1, 0, 1, ...: after step k exactly the
    //    vertices with M <= k are labelled. O(N + E) per column, no tie-breaking involved.
    s.m.assign(n, kUnset);
    auto for_tight_in = [&](uint32_t v, auto&& fn) {
        const uint64_t dv = s.dist[v];
        for (uint32_t e = in.offsets[v]; e < in.offsets[v + 1]; ++e) {
            const uint32_t a = in.src[e];
            if (a == j || s.dist[a] == kInf || s.dist[a] != dv + in.w[e]) continue;
            fn(a);
        }
    };
    for_tight_in(j, [&](uint32_t a) { s.m[a] = -1; });
    for (uint32_t k = 0; k < n; ++k) {
        if (k == j || s.m[k] == kUnset) continue;
        const int32_t kv = static_cast<int32_t>(k);
        s.stack.clear();
        s.stack.push_back(k);
        while (!s.stack.empty()) {
            const uint32_t x = s.stack.back();
            s.stack.pop_back();
            for_tight_in(x, [&](uint32_t a) {
                if (s.m[a] != kUnset) return;
                s.m[a] = kv;
                if (a < k) s.stack.push_back(a);  // already activated: expand now
            });
        }
    }
    // 3) FW predecessor: pred(a) = a if M(a) == -1 else pred(M(a)); memoised chain walk.
    s.pred.assign(n, kUnknown);
    for (uint32_t a = 0; a < n; ++a) {
        if (a == j) continue;
        if (s.dist[a] == kInf) {
            s.pred[a] = kNoPred;
            continue;
        }
        if (s.pred[a] != kUnknown) continue;
        s.chain.clear();
        uint32_t x = a;
        int32_t r = kNoPred;
        for (uint32_t steps = 0; steps <= n; ++steps) {
            if (s.pred[x] != kUnknown) {
                r = s.pred[x];
                break;
            }
            s.chain.push_back(x);
            if (s.m[x] == -1) {
                r = static_cast<int32_t>(x);
                break;
            }
            if (s.m[x] == kUnset) break;  // unreachable by construction (finite D => M set)
            x = static_cast<uint32_t>(s.m[x]);
        }
        for (uint32_t y : s.chain) s.pred[y] = r;
    }
    for (uint32_t a = 0; a < n; ++a) {
        const uint64_t d = s.dist[a];
        W[size_t(a) * n + j] = d == kInf ? kUnlinked : static_cast<uint32_t>(d);
        P[size_t(a) * n + j] = a == j ? kNoPred : s.pred[a];
    }
}

}  // namespace

void fast_raw(const RawGraph& g, AllPairsMatrices& out, const ParallelFor& pf) {
    const uint32_t n = g.n;
    out.resize(n);
    if (!g.node.empty()) out.mutable_nodes().assign(g.node.begin(), g.node.end());
    InEdges in;
    in.offsets.assign(n + 1, 0);
    const uint32_t m = g.offsets[n];
    for (uint32_t e = 0; e < m; ++e) ++in.offsets[g.nbr[e] + 1];
    for (uint32_t v = 0; v < n; ++v) in.offsets[v + 1] += in.offsets[v];
    in.src.resize(m);
    in.w.resize(m);
    {
        std::vector<uint32_t> fill(in.offsets.begin(), in.offsets.end() - 1);
        for (uint32_t a = 0; a < n; ++a)
            for (uint32_t e = g.offsets[a]; e < g.offsets[a + 1]; ++e) {
                const uint32_t slot = fill[g.nbr[e]]++;
                in.src[slot] = a;
                in.w[slot] = g.w[e];
            }
    }
    uint32_t* W = out.mutable_weights().data();
    int32_t* P = out.mutable_preds().data();
    pf(n, 4, [&](uint32_t begin, uint32_t end) {
        ColumnScratch s;
        for (uint32_t j = begin; j < end; ++j) solve_column(in, n, j, s, W, P);
    });
}

}  // namespace detail

void reference_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                         const ParallelFor& pf) {
    detail::RawOwned o;
    detail::build_raw(graph, input, o);
    detail::reference_raw(o.view(), out, pf);
}

namespace {
AllPairsMethod run_raw(const detail::RawGraph& g, AllPairsMatrices& out, const ParallelFor& pf, uint32_t min_n) {
    if (g.n >= min_n && detail::fast_path_eligible(g)) {
        detail::fast_raw(g, out, pf);
        return AllPairsMethod::Fast;
    }
    detail::reference_raw(g, out, g.n >= kParallelReferenceMinPlanets ? pf : ParallelFor(serial_for));
    return AllPairsMethod::Reference;
}
}  // namespace

AllPairsMethod fast_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                              const ParallelFor& pf) {
    return compute_all_pairs(graph, input, out, pf, 0);
}

AllPairsMethod compute_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                                 const ParallelFor& pf, uint32_t fast_min_planets) {
    detail::RawOwned o;
    detail::build_raw(graph, input, o);
    return run_raw(o.view(), out, pf, fast_min_planets);
}

bool AllPairsCache::update(const PlanetGraph& graph, const AllPairsInput& input, const ParallelFor& pf) {
    detail::RawOwned o;
    std::vector<uint32_t> cost;
    std::vector<uint8_t> hidden;
    detail::build_raw(graph, input, o, &cost, &hidden);
    std::vector<float> zoff(input.z_offset.begin(), input.z_offset.end());
    const bool same_z = zoff.size() == zoff_.size() &&
                        (zoff.empty() || std::memcmp(zoff.data(), zoff_.data(), zoff.size() * sizeof(float)) == 0);
    if (valid_ && graph_fp_ == graph.fingerprint() && cost == cost_ && hidden == hidden_ && same_z)
        return false;
    method_ = run_raw(o.view(), m_, pf, fast_min_);
    valid_ = true;
    graph_fp_ = graph.fingerprint();
    cost_ = std::move(cost);
    hidden_ = std::move(hidden);
    zoff_ = std::move(zoff);
    ++recomputes_;
    return true;
}

}  // namespace alamo::galactic
