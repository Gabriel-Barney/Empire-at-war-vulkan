#include "alamo/ai/tactical/zone_reachability.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/ascii.hpp"

namespace alamo::ai::tactical {

namespace {

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
        if (ascii_iequals(zones_[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

void ZoneGraph::build(std::span<const HintZone> zones, const ParallelFor& pf) {
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
    // A1_READINGS §25.2: the global undirected zone graph (0x5a7da0): zone i's links to a higher index, any flags.
    flood_nbr_.assign(zones_.size(), {});
    size_t li = 0;
    for (uint32_t i = 0; i < zones_.size(); ++i)
        for (size_t k = 0; k < zones_[i].links.size(); ++k) {
            const int32_t j = link_target_[li++];
            if (j <= static_cast<int32_t>(i)) continue;
            flood_nbr_[i].push_back(static_cast<uint32_t>(j));
            flood_nbr_[static_cast<size_t>(j)].push_back(i);
        }
    node_cost_.assign(zones_.size(), 0u);
    recompute(pf);
}

void ZoneGraph::flood(int32_t start, const std::function<bool(uint32_t)>& passes, std::vector<uint32_t>& out) const {
    out.clear();
    const uint32_t n = size();
    if (start < 0 || static_cast<uint32_t>(start) >= n || flood_nbr_.size() != n) return;
    std::vector<uint8_t> pass(n, 2);  // 2 = not asked yet
    const auto ok = [&](uint32_t z) {
        if (pass[z] == 2) pass[z] = passes(z) ? 1 : 0;
        return pass[z] == 1;
    };
    std::vector<uint8_t> visited(n, 0);
    const auto bfs = [&](uint32_t s, std::vector<uint32_t>& res) {
        res.push_back(s);
        visited[s] = 1;
        for (size_t q = 0; q < res.size(); ++q)
            for (const uint32_t nb : flood_nbr_[res[q]]) {
                if (visited[nb]) continue;
                visited[nb] = 1;
                if (ok(nb)) res.push_back(nb);
            }
    };
    const auto s = static_cast<uint32_t>(start);
    if (ok(s)) {
        bfs(s, out);
        return;
    }
    visited[s] = 1;
    std::vector<uint32_t> part;
    for (const uint32_t nb : flood_nbr_[s]) {
        if (visited[nb]) continue;
        if (!ok(nb)) {
            visited[nb] = 1;
            continue;
        }
        part.clear();
        bfs(nb, part);
        if (part.size() > out.size()) out = part;
    }
}

bool ZoneGraph::links_to(uint32_t from, uint32_t to, uint32_t mask) const {
    if (from >= size()) return false;
    size_t li = 0;
    for (uint32_t i = 0; i < from; ++i) li += zones_[i].links.size();
    for (const HintZone::Link& l : zones_[from].links)
        if (link_target_[li++] == static_cast<int32_t>(to) && (l.kind & mask) != 0) return true;
    return false;
}

std::vector<uint32_t> ZoneGraph::threat_node_costs(const ZoneThreatFn& threat) const {
    std::vector<uint32_t> cost(zones_.size(), 0u);
    if (threat)  // Node_Weight (vf3), node index order
        for (uint32_t i = 0; i < size(); ++i) cost[i] = land_zone_node_cost(threat(i));
    return cost;
}

void ZoneGraph::recompute_with_threat(const ZoneThreatFn& threat, const ParallelFor& pf) {
    node_cost_ = threat_node_costs(threat);
    recompute(pf);
}

void ZoneGraph::build_raw(std::span<const uint32_t> node_cost, ::alamo::galactic::detail::RawOwned& raw) const {
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
            const uint32_t cost = j < node_cost.size() ? node_cost[j] : 0u;
            const uint32_t w = land_zone_edge_weight(zones_[from], zones_[j], cost);
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
    raw = {};
    raw.n = n;
    raw.offsets.assign(size_t(n) + 1, 0);
    for (uint32_t i = 0; i < n; ++i) {
        raw.offsets[i + 1] = raw.offsets[i] + static_cast<uint32_t>(out[i].size());
        for (const Edge& e : out[i]) {
            raw.nbr.push_back(e.to);
            raw.w.push_back(e.w);
        }
    }
    raw.node.assign(n, 1);
}

void ZoneGraph::recompute(const ParallelFor& pf) {
    build_raw(node_cost_, raw_);
    ::alamo::galactic::detail::reference_raw(raw_.view(), m_, pf);
}

void ZoneGraph::compute(std::span<const uint32_t> node_cost, ZoneGraphResult& out, const ParallelFor& pf) const {
    out.node_cost.assign(node_cost.begin(), node_cost.end());
    build_raw(out.node_cost, out.raw);
    ::alamo::galactic::detail::reference_raw(out.raw.view(), out.m, pf);
}

void ZoneGraph::adopt(ZoneGraphResult& r) {
    std::swap(node_cost_, r.node_cost);
    std::swap(raw_, r.raw);
    std::swap(m_, r.m);
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
                              const TacticalAiConstants& c) {
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
