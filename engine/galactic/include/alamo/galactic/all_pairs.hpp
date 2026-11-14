// AI travel planning all-pairs (GALACTIC.md section 4, AIExecutionPathFinderClass), exact to
// docs/re/PHASE2_ANSWERS.md Q7-Q10.
//
//   reference_all_pairs : the original's Floyd-Warshall (Q9), uint32 wrapping arithmetic, O(N^3).
//                         Optionally parallel over rows i for a fixed k (exact: row k and column
//                         k never change during phase k, see all_pairs.cpp).
//   fast_all_pairs      : per-target reverse Dijkstra + FW-equivalent predecessor derivation,
//                         O(N * E log N), parallel over targets. Used ONLY when it provably gives
//                         identical matrices (fast_path_eligible); otherwise falls back to the
//                         reference.
//   compute_all_pairs   : picks fast only when eligible AND N >= the measured crossover; the
//                         reference runs row-parallel from kParallelReferenceMinPlanets.
//   AllPairsCache       : one AI's matrices; recomputes only when an input changes.
//
// When is the fast path exact? (fast_path_eligible)
//   FW forms every candidate sum W[i][k] + W[k][j] mod 2^32 and compares unsigned against
//   W[i][j], with 0xFFFFFFFF doubling as "unlinked". Let B = 2 * sum over vertices v of the
//   largest weight of an edge INTO v. Claim: if B <= 0xFFFFFFFE, every value FW ever stores or
//   forms is an exact (non-wrapped) walk weight <= B, so FW equals FW over exact integers with
//   "unlinked" = +infinity. Induction over the FW steps: while no wrap has happened, each stored
//   W_k(i,j) is the exact minimum walk weight with intermediates <= k; weights are >= 0, so that
//   minimum is attained by a simple path, which enters each vertex at most once, so W <= B/2.
//   A candidate is the concatenation of two such paths, so it enters each vertex at most twice:
//   candidate <= B <= 0xFFFFFFFE. Hence no wrap, the unsigned compare equals the exact compare,
//   and no finite value equals 0xFFFFFFFF (so "skip if -1" only ever skips true no-paths). An
//   edge whose own weight is 0xFFFFFFFF (Q8 quirk) is not an edge in BOTH algorithms (dropped
//   when the raw graph is built), so it does not enter the argument.
//   Under that condition the previous equivalence proof applies unchanged: with strict '<' and
//   weights >= 0, row/column k are fixed in phase k, so pred[i][j] (i != j) is last set in phase
//   m = M(i,j) := min over shortest i->j walks of the largest intermediate index (-1 if the
//   direct edge is already shortest), to FW's pred[m][j] after phase m-1, which by the subpath
//   argument is determined by M(m,j) < m. So pred[i][j] = last a of the chain a0 = i,
//   a_{t+1} = M(a_t, j) until M = -1. The new conventions (Q9) do not disturb it: the diagonal
//   (weight 0, pred -1) is never relaxed (no sum is < 0) and never copied (relaxing W[i][k] via
//   j == k would need W[i][k] + 0 < W[i][k]); off-diagonal init pred = row index i is the same
//   as before. Graphs are directed now (Layer_Z_Adjust makes W asymmetric), which the proof never
//   assumed; the fast path walks explicit in-edges.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/galactic/bitset.hpp"
#include "alamo/galactic/planet_graph.hpp"

namespace alamo::galactic {

inline constexpr uint32_t kUnlinked = 0xFFFFFFFFu;  // weight "-1"
inline constexpr int32_t kNoPred = -1;
// Below this many planets the reference FW is faster than the fast path (bench crossover,
// 128-200 depending on thread count).
inline constexpr uint32_t kFastAllPairsMinPlanets = 150;
// compute_all_pairs / AllPairsCache hand `pf` to the row-parallel reference only from this size
// (one ParallelFor call per k: the fan-out overhead dominates small matrices).
inline constexpr uint32_t kParallelReferenceMinPlanets = 256;

// Q7: node cost = (uint32)(low 32 bits of (int64)trunc(value)) / 100u. `value` is the AI
// perception total (perception + opposing strength when the AI does not hold the planet). No
// clamping: negative totals wrap (-50 => 42949672). NaN / outside int64 => 0 (cvttsd2si
// "integer indefinite" 0x8000000000000000 has zero low bits). An AI player with no AI system
// gets Node_Weight 0xFFFFFFFF => node_cost_from_raw(0xFFFFFFFF) = 42949672.
uint32_t node_cost_from_value(double value);
inline constexpr uint32_t node_cost_from_raw(uint32_t node_weight) { return node_weight / 100u; }

// Q8 step 4: hop = (int64)trunc(travel * 10.0f) & 0xFFFF, travel = distance or distance / HS
// (float divide first, then the float multiply). NaN / outside int64 => 0.
uint16_t hop_from_distance(float travel);

// Q8 step 5: node_cost(destination) * 65536 + hop, 32-bit wrapping.
inline constexpr uint32_t pack_edge_weight(uint32_t node_cost, uint16_t hop) {
    return node_cost * 65536u + hop;
}

struct AllPairsInput {
    // Raw perception totals (double) per planet; converted with node_cost_from_value. Missing
    // entries => node cost 0. Q7 assumes element i of the AI's planet-target list is planet i; the
    // caller provides that.
    std::span<const double> node_value;
    // Q8 step 2 fog: per-planet Is_Planet_Revealed (PlayerView::is_planet_revealed). Pass it ONLY
    // when the AI player has an AI system and is not human; nullptr or empty => no fog gate.
    // An edge touching an unrevealed planet is unlinked.
    const Bitset* revealed = nullptr;
    // Q2/Q8 step 1: Layer_Z_Adjust of each planet's TYPE. For the pair (i, j) the SOURCE i's
    // value (when non-zero) is added to the DESTINATION's z, for both the free-hop test and the
    // cost; W becomes asymmetric. Empty / missing entries => 0.
    std::span<const float> z_offset;
};

class AllPairsMatrices {
public:
    uint32_t size() const { return n_; }
    uint32_t weight(uint32_t i, uint32_t j) const { return weight_[size_t(i) * n_ + j]; }
    int32_t pred(uint32_t i, uint32_t j) const { return pred_[size_t(i) * n_ + j]; }
    std::span<const uint32_t> weights() const { return weight_; }
    std::span<const int32_t> preds() const { return pred_; }
    bool is_node(uint32_t p) const { return p < n_ && node_[p] != 0; }

    // Get_Path (Q10). src == dst => [src, src], weights [0, 0]. Either planet not a node, or
    // unreachable => both empty. Otherwise the predecessor walk from dst; hop_weights[k] =
    // weight(path[k], path[k+1]) (per hop, post-FW), last entry 0. O(L). Returns !path.empty().
    // Wrapped matrices (fast_path_eligible false, e.g. negative perception totals) can hold a
    // CYCLIC predecessor chain; the original's Get_Path never terminates there (hang / OOM).
    // Here that case returns empty (deviation). A chain that ends at a vertex u != from (pred -1)
    // is returned as the original would: path.front() == u.
    bool get_path(uint32_t from, uint32_t to, std::vector<PlanetIndex>& path,
                  std::vector<uint32_t>* hop_weights = nullptr) const;

    void resize(uint32_t n);  // all unlinked, all nodes
    std::vector<uint32_t>& mutable_weights() { return weight_; }
    std::vector<int32_t>& mutable_preds() { return pred_; }
    std::vector<uint8_t>& mutable_nodes() { return node_; }
    friend bool operator==(const AllPairsMatrices&, const AllPairsMatrices&) = default;

private:
    uint32_t n_ = 0;
    std::vector<uint32_t> weight_;
    std::vector<int32_t> pred_;
    std::vector<uint8_t> node_;
};

enum class AllPairsMethod : uint8_t { Reference, Fast };

void reference_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                         const ParallelFor& pf = serial_for);
// Fast path when fast_path_eligible, else the reference. Returns the method actually used.
AllPairsMethod fast_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                              const ParallelFor& pf = serial_for);
// Fast only when eligible AND graph.size() >= fast_min_planets.
AllPairsMethod compute_all_pairs(const PlanetGraph& graph, const AllPairsInput& input, AllPairsMatrices& out,
                                 const ParallelFor& pf = serial_for,
                                 uint32_t fast_min_planets = kFastAllPairsMinPlanets);

// Owns one AI's matrices. update() is a pure function of (graph fingerprint, node costs, fog
// mask, z offsets) and can run on a worker; it recomputes only when one of them changed (exact
// comparison of stored copies, not a hash), and returns true when it recomputed. When to call
// it is the caller's business (see cadence.hpp for the original's 90-frame schedule).
class AllPairsCache {
public:
    bool update(const PlanetGraph& graph, const AllPairsInput& input, const ParallelFor& pf = serial_for);
    void invalidate() { valid_ = false; }
    void set_fast_min_planets(uint32_t n) { fast_min_ = n; }
    const AllPairsMatrices& matrices() const { return m_; }
    uint32_t recompute_count() const { return recomputes_; }
    AllPairsMethod last_method() const { return method_; }

private:
    bool valid_ = false;
    uint64_t graph_fp_ = 0;
    std::vector<uint32_t> cost_;
    std::vector<uint8_t> hidden_;
    std::vector<float> zoff_;
    AllPairsMatrices m_;
    uint32_t recomputes_ = 0;
    uint32_t fast_min_ = kFastAllPairsMinPlanets;
    AllPairsMethod method_ = AllPairsMethod::Reference;
};

namespace detail {
// Directed graph with final edge weights (out-edges in CSR, neighbours ascending, no self loops,
// no 0xFFFFFFFF weights). `node` marks graph nodes (for get_path); may be empty => all nodes.
struct RawGraph {
    uint32_t n = 0;
    std::span<const uint32_t> offsets;  // n + 1
    std::span<const uint32_t> nbr;
    std::span<const uint32_t> w;
    std::span<const uint8_t> node;
};
struct RawOwned {
    uint32_t n = 0;
    std::vector<uint32_t> offsets, nbr, w;
    std::vector<uint8_t> node;
    RawGraph view() const { return RawGraph{n, offsets, nbr, w, node}; }
};
// Edge_Weight (Q8) for every ordered pair of nodes; unlinked pairs (and the 0xFFFFFFFF quirk)
// produce no edge. Also returns the node costs / hidden mask it used (for the cache).
void build_raw(const PlanetGraph& graph, const AllPairsInput& input, RawOwned& out,
               std::vector<uint32_t>* cost_out = nullptr, std::vector<uint8_t>* hidden_out = nullptr);
uint64_t walk_bound(const RawGraph& g);  // B of the header comment
inline bool fast_path_eligible(const RawGraph& g) { return walk_bound(g) <= 0xFFFFFFFEull; }
void reference_raw(const RawGraph& g, AllPairsMatrices& out, const ParallelFor& pf = serial_for);
// Precondition: fast_path_eligible(g). Undefined results otherwise.
void fast_raw(const RawGraph& g, AllPairsMatrices& out, const ParallelFor& pf);
}  // namespace detail

}  // namespace alamo::galactic
