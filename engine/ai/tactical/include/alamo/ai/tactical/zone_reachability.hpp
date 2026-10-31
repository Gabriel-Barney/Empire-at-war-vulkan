// A1 - land reachability (ZoneReachabilityClass slot 7, 0x73f5f0), docs/re/AI_TACTICAL.md 3.4.
//
// The land execution path finder (LandExecutionPathFinderClass, created at Execution slot 6) is a
// zone graph with the AIExecutionPathFinderClass all-pairs: Recompute_All_Pairs once at creation,
// then every 90 frames, staggered per player (PHASE2_ANSWERS 9-11). ZoneGraph reuses the galactic
// reference Floyd-Warshall (uint32 wrapping, strict unsigned compare, k/i/j) and Get_Path.
//
// Graph (TACTICAL_GAPS §6.1, LandExecutionPathFinderClass vf1-vf3) [H; M link direction]: nodes = hint zones in list
// order; Edge_Weight(i -> j) exists only when zone j's link table holds a link to zone i with non-zero flags (self links
// and unknown ids dropped; duplicates keep the smallest weight [port]); then hop = (uint)(int64)(3-D distance between
// the zone centres / 10.0) & 0xFFFF and the weight = n(j) x 65536 + hop, or 0x10000 + hop when the node cost n(j) = 0
// (uint32 wrap). Node cost n(i) = (uint32)(int64)max(0, threat sum over zone i's cells) / 100 for the AI player
// (land_zone_node_cost; the threat grid's cells_danger), 0 without a threat grid. A ZoneEdgeWeightFn replaces the
// weight (tests).
//
// zone_reachability_cost [H unless tagged]: only reachability levels 6-10 (No/Low/Medium/High/
// Any_Threat) are answered; every other level returns kUnreachable (so a land region is filtered out
// by Reachable_Target with e.g. "Any") [H]. Tolerance: No_Threat 0, Low/Medium/High the
// GameConstants values, Any_Threat 1e18. PROVISIONAL [L]: "the path's threat stays below the
// tolerance" is implemented as: every zone after the source on Get_Path (the destination included;
// for source == destination the zone itself) has threat <= tolerance; paths longer than 1000 zones
// are refused [M]. The threat per zone is an input (threat grid, AI_TACTICAL_PERCEPTION.md). The
// returned cost is the all-pairs weight (double).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/tactical/applies.hpp"
#include "alamo/ai/tactical/constants.hpp"
#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/galactic/all_pairs.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}

namespace alamo::ai::tactical {

// Edge weight for the directed edge from zone index `from` to `to` (`link_kind` = the flags of zone `to`'s link to
// `from`). Return ::alamo::galactic::kUnlinked (0xFFFFFFFF) to drop the edge.
using ZoneEdgeWeightFn = std::function<uint32_t(uint32_t from, uint32_t to, uint32_t link_kind)>;
// Threat of a zone index for the AI player (threat grid; >= 0).
using ZoneThreatFn = std::function<double(uint32_t zone)>;

// TACTICAL_GAPS §6.1 [H]: hop part of Edge_Weight(a -> b) and the full weight with b's node cost.
uint32_t land_zone_hop(const HintZone& a, const HintZone& b);
uint32_t land_zone_edge_weight(const HintZone& a, const HintZone& b, uint32_t node_cost_b);
// Node cost from a zone's threat sum: (uint32)(int64)max(0, sum) / 100.
uint32_t land_zone_node_cost(double threat_sum);
// Weight with node cost 0 (no threat grid / no threat): 0x10000 + hop.
uint32_t default_zone_edge_weight(const HintZone& a, const HintZone& b);

class ZoneGraph {
public:
    // Builds the graph and runs Recompute_All_Pairs (node costs 0). `pf` parallelises rows for a fixed k (exact).
    void build(std::span<const HintZone> zones, const ZoneEdgeWeightFn& weight = {}, const ParallelFor& pf = serial_for);
    // Recompute with new edge weights (the 90-frame cadence is the caller's: cadence frames of
    // PHASE2_ANSWERS 11). Without `weight` the §6.1 weight with the stored node costs is used.
    void recompute(const ZoneEdgeWeightFn& weight = {}, const ParallelFor& pf = serial_for);
    // Node_Weight pass + Recompute_All_Pairs: node cost of every zone from `threat` (land_zone_node_cost), then the §6.1
    // weights. An empty `threat` = no threat grid (every cost 0).
    void recompute_with_threat(const ZoneThreatFn& threat, const ParallelFor& pf = serial_for);
    std::span<const uint32_t> node_costs() const { return node_cost_; }

    uint32_t size() const { return static_cast<uint32_t>(zones_.size()); }
    const HintZone& zone(uint32_t i) const { return zones_[i]; }
    int32_t index_of_id(uint32_t id) const;
    // First zone with this name, case-insensitive. Diagnostics only: 11 shipped land maps repeat a zone name and the
    // original links zones by 02 id, never by name (TACTICAL_GAPS 6.8); use index_of_id for links.
    int32_t index_of_name(std::string_view name) const;
    // PROVISIONAL [L] locator (the land mode's position -> zone slot +0x180 is not traced): the zone
    // with the nearest centre in XY (squared distance, float); ties -> lowest index; -1 if no zones.
    int32_t nearest_zone(float x, float y) const;

    uint32_t weight(uint32_t from, uint32_t to) const { return m_.weight(from, to); }
    // Get_Path (PHASE2_ANSWERS 10) over zone indices. False if unreachable / invalid.
    bool path(uint32_t from, uint32_t to, std::vector<uint32_t>& zones) const;
    uint32_t edge_count() const { return static_cast<uint32_t>(raw_.nbr.size()); }
    uint32_t unresolved_links() const { return unresolved_; }
    const ::alamo::galactic::AllPairsMatrices& matrices() const { return m_; }

    void hash(StateHasher& h) const;  // zone count, then W and P row-major
    // N1 save / load: u32 version (1), u32 zone count, the node costs, u64 StateHasher digest of hash(). The zones, links
    // and edges are static (build()); the all-pairs matrices are rebuilt by read() (node costs + recompute()) and must
    // match the saved digest. read() on a graph built from the same zones; false and unchanged otherwise.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<HintZone> zones_;
    std::vector<int32_t> link_target_;  // per (zone, link) flattened: resolved index or -1
    std::vector<uint32_t> node_cost_;   // per zone (Node_Weight / 100)
    ::alamo::galactic::detail::RawOwned raw_;
    ::alamo::galactic::AllPairsMatrices m_;
    uint32_t unresolved_ = 0;
};

// Threat tolerance of a reachability level (levels 6-10); negative for the other levels.
double reachability_tolerance(Reachability level, const TacticalAiConstants& c);

// Path cost from `source_zone` to `target_zone`, or kUnreachable.
double zone_reachability_cost(const ZoneGraph& graph, int32_t source_zone, int32_t target_zone, Reachability level,
                              const TacticalAiConstants& c, const ZoneThreatFn& threat = {});

}  // namespace alamo::ai::tactical
