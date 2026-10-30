// Shared, immutable planet graph (GALACTIC.md section 3 "Build_Planet_Graph", corrected by
// docs/re/PHASE2_ANSWERS.md Q1-Q3).
//
// Built once from a GalaxyMap and shared read-only by every player's fleet A* and every AI's
// all-pairs computation. Rebuild when the link set or the node set changes (story rewards, a
// planet's owner becoming / leaving -1).
//
// Node index == PlanetIndex (row order of GalaxyMap::planets), so callers never remap. Planets
// that are not graph nodes keep their row but are isolated (no edges) and every query on them
// fails. Because the original's node order is the planet query order (assumed == <Locations>
// order, PHASE2_ANSWERS "Not resolved: Planet order") and non-nodes are isolated, ascending
// PlanetIndex order is the original's ascending node order.
//
// Edges (CSR, both directions stored): one for every unordered pair {a, b} of nodes that has a
// trade route OR whose 3-D float distance <= max_fleet_move_distance (Q2, inclusive). Neighbours
// of a node are sorted by planet index == the original's edge-vector order (Q4).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/galactic/bitset.hpp"

namespace alamo::galactic {

inline constexpr uint32_t kNoRoute = 0xFFFFFFFFu;

struct PlanetGraphOptions {
    // Q1: the node query keeps objects that have a PLANET behaviour slot AND an owner id != -1.
    // The owner is campaign state: `owned` (indexed by PlanetIndex) clear => owner == -1 => not a
    // node. nullptr or empty => every planet counts as owned.
    const Bitset* owned = nullptr;
    // Q1: the original does NOT test In_Background; scenery is excluded only because it has no
    // PLANET behaviour (kPlanetHasPlanetBehavior). Overrides for tests / mods:
    bool require_planet_behavior = true;  // false: ignore kPlanetHasPlanetBehavior
    bool exclude_background = false;      // true: also drop kPlanetInBackground (pre-Q1 rule)
    // Q3: the max HS_Speed_Factor tracker starts at 1.0f in the singleton's constructor and is
    // never reset by a rebuild. Pass the previous graph's max_speed_factor() when rebuilding to
    // keep that monotonic lifetime behaviour.
    float max_speed_factor_floor = 1.0f;
};

// 3-D Euclidean distance as the original computes it (Q2): sqrtf(dx*dx + dy*dy + dz*dz), float,
// left-to-right, no FMA. Bitwise symmetric in (a, b).
float planet_distance(const Vec3& a, const Vec3& b);

class PlanetGraph {
public:
    PlanetGraph() = default;
    // pf may run the per-planet row builds concurrently; the result does not depend on it.
    static PlanetGraph build(const GalaxyMap& map, const PlanetGraphOptions& options = {},
                             const ParallelFor& pf = serial_for);

    uint32_t size() const { return n_; }                 // planet rows (node index space)
    uint32_t node_count() const { return node_count_; }  // N of the original (heap / pool sizes)
    uint32_t edge_slots() const { return static_cast<uint32_t>(nbr_.size()); }  // directed

    bool included(PlanetIndex p) const { return p < n_ && included_[p] != 0; }  // is a node
    const Vec3& position(PlanetIndex p) const { return position_[p]; }

    // Edge slots of p are [edge_begin(p), edge_end(p)).
    uint32_t edge_begin(PlanetIndex p) const { return offsets_[p]; }
    uint32_t edge_end(PlanetIndex p) const { return offsets_[p + 1]; }
    PlanetIndex neighbour(uint32_t e) const { return nbr_[e]; }
    float distance(uint32_t e) const { return dist_[e]; }
    uint32_t route(uint32_t e) const { return route_[e]; }        // kNoRoute for a free hop
    float speed_factor(uint32_t e) const { return speed_[e]; }    // route HS_Speed_Factor, 1 for free hop
    float travel_cost(uint32_t e) const { return cost_[e]; }      // Q6 base cost: dist, or dist / HS with a route

    // Q3: max(max_speed_factor_floor, HS of every linked route), raised only on strictly greater.
    float max_speed_factor() const { return max_speed_factor_; }
    float max_fleet_move_distance() const { return max_move_; }

    // Content fingerprint (FNV-1a over all tables): equal graphs => equal value. Used by caches.
    uint64_t fingerprint() const { return fingerprint_; }

    std::span<const uint32_t> offsets() const { return offsets_; }
    std::span<const PlanetIndex> neighbours() const { return nbr_; }

private:
    uint32_t n_ = 0;
    uint32_t node_count_ = 0;
    std::vector<uint8_t> included_;
    std::vector<Vec3> position_;
    std::vector<uint32_t> offsets_{0};
    std::vector<PlanetIndex> nbr_;
    std::vector<float> dist_;
    std::vector<uint32_t> route_;
    std::vector<float> speed_;
    std::vector<float> cost_;
    float max_speed_factor_ = 1.0f;
    float max_move_ = 0.0f;
    uint64_t fingerprint_ = 0;
};

}  // namespace alamo::galactic
