#include "alamo/ai/tactical/zone_reachability.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

namespace {

char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}

constexpr size_t kMaxPathEntries = 1000;  // A1_READINGS §3.3: entries summed (not a path-length refusal)

}  // namespace

// TACTICAL_GAPS §6.1 [H]: hop = (uint)(int64)(3-D centre distance / 10.0) & 0xFFFF [port: float distance, double divide].
uint32_t land_zone_hop(const HintZone& a, const HintZone& b) {
    const float dx = b.center[0] - a.center[0], dy = b.center[1] - a.center[1], dz = b.center[2] - a.center[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double q = static_cast<double>(d) / 10.0;
    if (!(q >= 0.0) || !(q < 9.0e18)) return 0xFFFFu;  // NaN / overflow guard [port]
    return static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(q)) & 0xFFFFu);
}

uint32_t land_zone_edge_weight(const HintZone& a, const HintZone& b, uint32_t node_cost_b) {
    const uint32_t hop = land_zone_hop(a, b);
    return node_cost_b == 0 ? 0x10000u + hop : node_cost_b * 65536u + hop;  // uint32 wrap as the original
}

uint32_t land_zone_node_cost(double threat_sum) {
    const double s = threat_sum > 0.0 ? threat_sum : 0.0;
    const int64_t v = s < 9.0e18 ? static_cast<int64_t>(s) : INT64_MAX;
    return static_cast<uint32_t>(v) / 100u;
}

uint32_t default_zone_edge_weight(const HintZone& a, const HintZone& b) { return land_zone_edge_weight(a, b, 0); }

int64_t ZoneLocator::cell_of(float x, float y) {
    const float fx = (x + 10.0f) / 20.0f, fy = (y + 10.0f) / 20.0f;
    if (!(fx > -1.0f) || !(fy > -1.0f) || !(fx < 65536.0f) || !(fy < 65536.0f)) return -1;  // NaN / outside u16 cells
    const auto cx = static_cast<int32_t>(fx), cy = static_cast<int32_t>(fy);  // truncation toward 0
    return static_cast<int64_t>(static_cast<uint32_t>(cx) | (static_cast<uint32_t>(cy) << 16));
}

void ZoneLocator::build(const std::vector<HintZone>& zones) {
    first_.clear();
    for (size_t i = 0; i < zones.size(); ++i)
        for (const uint32_t c : zones[i].cells) first_.emplace_back(c, static_cast<int32_t>(i));
    // stable: equal cells keep zone list order, so unique() keeps the first zone holding the cell
    std::stable_sort(first_.begin(), first_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    first_.erase(std::unique(first_.begin(), first_.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                 first_.end());
}

int32_t ZoneLocator::zone_at(float x, float y) const {
    const int64_t c = cell_of(x, y);
    if (c < 0) return -1;
    const auto key = static_cast<uint32_t>(c);
    const auto it = std::lower_bound(first_.begin(), first_.end(), key, [](const auto& e, uint32_t k) { return e.first < k; });
    return it != first_.end() && it->first == key ? it->second : -1;
}

int32_t ZoneGraph::index_of_id(uint32_t id) const {
    for (size_t i = 0; i < zones_.size(); ++i)
        if (zones_[i].id == id) return static_cast<int32_t>(i);  // first zone with that id
    return -1;
}

int32_t ZoneGraph::index_of_name(std::string_view name) const {
    for (size_t i = 0; i < zones_.size(); ++i)
        if (iequals(zones_[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

void ZoneGraph::build(std::span<const HintZone> zones, const ZoneEdgeWeightFn& weight, const ParallelFor& pf) {
    zones_.assign(zones.begin(), zones.end());
    locator_.build(zones_);
    link_target_.clear();
    unresolved_ = 0;
    for (const HintZone& z : zones_) {
        for (const HintZone::Link& l : z.links) {
            const int32_t j = index_of_id(l.zone);
            if (j < 0) ++unresolved_;
            link_target_.push_back(j);
        }
    }
    node_cost_.assign(zones_.size(), 0u);
    recompute(weight, pf);
}

void ZoneGraph::recompute_with_threat(const ZoneThreatFn& threat, const ParallelFor& pf) {
    node_cost_.assign(zones_.size(), 0u);
    if (threat)  // Node_Weight (vf3), node index order
        for (uint32_t i = 0; i < size(); ++i) node_cost_[i] = land_zone_node_cost(threat(i));
    recompute({}, pf);
}

void ZoneGraph::recompute(const ZoneEdgeWeightFn& weight, const ParallelFor& pf) {
    const uint32_t n = size();
    struct Edge {
        uint32_t to, w;
    };
    std::vector<std::vector<Edge>> out(n);
    // Edge i -> j exists when zone j has a link to i with non-zero flags (TACTICAL_GAPS §6.1 [H; M direction]).
    size_t li = 0;
    for (uint32_t j = 0; j < n; ++j) {
        for (const HintZone::Link& l : zones_[j].links) {
            const int32_t i = link_target_[li++];
            if (i < 0 || static_cast<uint32_t>(i) == j || l.kind == 0) continue;
            const auto from = static_cast<uint32_t>(i);
            const uint32_t w = weight ? weight(from, j, l.kind) : land_zone_edge_weight(zones_[from], zones_[j], node_cost_[j]);
            if (w == ::alamo::galactic::kUnlinked) continue;
            out[from].push_back({j, w});
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        // CSR wants ascending neighbours, one entry per pair: keep the smallest weight [port].
        std::sort(out[i].begin(), out[i].end(), [](const Edge& a, const Edge& b) {
            return a.to != b.to ? a.to < b.to : a.w < b.w;
        });
        out[i].erase(std::unique(out[i].begin(), out[i].end(), [](const Edge& a, const Edge& b) { return a.to == b.to; }),
                     out[i].end());
    }
    raw_ = {};
    raw_.n = n;
    raw_.offsets.assign(size_t(n) + 1, 0);
    for (uint32_t i = 0; i < n; ++i) {
        raw_.offsets[i + 1] = raw_.offsets[i] + static_cast<uint32_t>(out[i].size());
        for (const Edge& e : out[i]) {
            raw_.nbr.push_back(e.to);
            raw_.w.push_back(e.w);
        }
    }
    raw_.node.assign(n, 1);
    ::alamo::galactic::detail::reference_raw(raw_.view(), m_, pf);
}

bool ZoneGraph::path(uint32_t from, uint32_t to, std::vector<uint32_t>& zones) const {
    return m_.get_path(from, to, zones);
}

void ZoneGraph::hash(StateHasher& h) const {
    h.u32(size());
    h.span(m_.weights());
    h.span(m_.preds());
}

double reachability_tolerance(Reachability level, const TacticalAiConstants& c) {
    switch (level) {
        case Reachability::NoThreat: return 0.0;
        case Reachability::LowThreat: return static_cast<double>(c.low_threat_tolerance);
        case Reachability::MediumThreat: return static_cast<double>(c.medium_threat_tolerance);
        case Reachability::HighThreat: return static_cast<double>(c.high_threat_tolerance);
        case Reachability::AnyThreat: return kUnreachable;
        default: return -1.0;
    }
}

double zone_reachability_cost(const ZoneGraph& graph, int32_t source_zone, int32_t target_zone, Reachability level,
                              const TacticalAiConstants& c, const ZoneThreatFn&) {
    const double tol = reachability_tolerance(level, c);
    if (tol < 0.0) return kUnreachable;
    const uint32_t n = graph.size();
    if (source_zone < 0 || static_cast<uint32_t>(source_zone) >= n) return kUnreachable;  // no source zone
    // A1_READINGS §3.3: S = uint32 wrapping sum of the first min(1000, len - 1) Get_Path hop weights; an empty path
    // (unreachable / unknown target) gives S = 0xFFFFFFFF.
    uint32_t sum = 0xFFFFFFFFu;
    std::vector<uint32_t> p, hops;
    if (target_zone >= 0 && static_cast<uint32_t>(target_zone) < n &&
        graph.matrices().get_path(static_cast<uint32_t>(source_zone), static_cast<uint32_t>(target_zone), p, &hops) &&
        !hops.empty()) {
        sum = 0;
        const size_t count = std::min(kMaxPathEntries, hops.size() - 1);
        for (size_t k = 0; k < count; ++k) sum += hops[k];
    }
    // §3.4: accept iff (float)((S >> 16) * 100) <= tolerance; cost = (float)(S & 0xFFFF) / 10.
    const auto threat = static_cast<float>((sum >> 16) * 100u);
    if (!(static_cast<double>(threat) <= tol)) return kUnreachable;
    return static_cast<double>(static_cast<float>(sum & 0xFFFFu) / 10.0f);
}

}  // namespace alamo::ai::tactical
