#include "alamo/ai/tactical/find_target.hpp"

#include <algorithm>

namespace alamo::ai::tactical {

bool valid_pick_probability(float p) { return p > 0.0f && p <= 1.0f; }

float pick_weight(float p) { return 1.0f / p - 1.0f; }

TargetPick select_target(std::span<const ScoredTarget> candidates, float w, SimRng& rng) {
    struct Entry {
        uint32_t target;
        float cum;
    };
    std::vector<Entry> list;
    float cum = 0.0f;
    auto append = [&](uint32_t t, float weight) {
        if (weight > 0.0f) {
            cum += weight;
            list.push_back({t, cum});
        }
    };
    int32_t best = -1;
    double best_score = 0.0;
    for (const ScoredTarget& c : candidates) {
        if (c.score <= best_score) {
            append(c.target, static_cast<float>(c.score));
        } else {
            if (best >= 0) append(static_cast<uint32_t>(best), static_cast<float>(best_score));
            best = static_cast<int32_t>(c.target);
            best_score = c.score;
        }
    }
    TargetPick out;
    out.target = best;
    if (list.empty() || w == 0.0f || best < 0) return out;
    append(static_cast<uint32_t>(best), cum / w);
    const float total = cum;
    if (!(total > 0.0f)) return out;
    const float r = rng.inline_range_float(0.0f, total);
    out.drew = true;
    const auto it = std::lower_bound(list.begin(), list.end(), r, [](const Entry& e, float v) { return e.cum < v; });
    out.target = static_cast<int32_t>(it != list.end() ? it->target : list.back().target);
    return out;
}

FindTargetReturn find_target_return_kind(const TacticalTarget* picked) {
    if (!picked) return FindTargetReturn::Nil;
    return picked->is_unit() ? FindTargetReturn::GameObject : FindTargetReturn::TargetLocation;
}

FindTargetReturn reachable_target_return_kind(const TacticalTarget* picked) {
    return picked ? FindTargetReturn::TargetLocation : FindTargetReturn::Nil;
}

void collect_candidates(std::span<const TacticalTarget> targets, const CandidateFilter& f, std::vector<ScoredTarget>& out) {
    out.clear();
    const float r2 = f.max_range * f.max_range;
    for (size_t i = 0; i < targets.size(); ++i) {
        const TacticalTarget& t = targets[i];
        if (f.applies && !f.applies(t)) continue;
        if (!f.score) continue;
        const std::optional<double> s = f.score(t);
        if (!s) continue;
        if (f.max_range > 0.0f) {
            const Vec3f p = f.position ? f.position(t) : t.position;
            const float dx = p.x - f.origin.x, dy = p.y - f.origin.y, dz = p.z - f.origin.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) continue;
        }
        if (f.reachability && t.has_reachability() && f.reachability(t) == kUnreachable) continue;
        out.push_back({static_cast<uint32_t>(i), *s});
    }
}

}  // namespace alamo::ai::tactical
