#include "alamo/tactical/presence/explosion_flinch.hpp"

#include <algorithm>

namespace alamo::tactical::presence {

float explosion_flinch_intensity(float intensity, float d2_xy, float range) {
    if (!(range > 0.0f)) return kMinExplosionFlinchIntensity;
    const float r2 = range * range;
    const float t = 1.0f - 0.5f * d2_xy / r2;  // left to right: (0.5 d²) / R² [M for the order]
    const float v = intensity * t;
    if (!(v >= kMinExplosionFlinchIntensity)) return kMinExplosionFlinchIntensity;  // NaN -> the floor too [port]
    return v > 1.0f ? 1.0f : v;
}

std::vector<ExplosionFlinch> explosion_flinches(float ex, float ey, float ez, float range, float intensity,
                                                std::span<const FlinchCandidate> candidates) {
    std::vector<ExplosionFlinch> out;
    if (!(range > 0.0f)) return out;
    std::vector<FlinchCandidate> c(candidates.begin(), candidates.end());
    std::stable_sort(c.begin(), c.end(), [](const FlinchCandidate& a, const FlinchCandidate& b) { return a.id < b.id; });
    for (size_t i = 0; i < c.size(); ++i) {
        if (i > 0 && c[i].id == c[i - 1].id) continue;  // first occurrence of an id wins
        const FlinchCandidate& v = c[i];
        if (!v.has_idle) continue;
        const float dx = v.x - ex, dy = v.y - ey, dz = v.z - ez;
        if (!(dx >= -range && dx <= range && dy >= -range && dy <= range && dz >= -range && dz <= range)) continue;
        const float d2 = dx * dx + dy * dy;
        out.push_back(ExplosionFlinch{v.id, dx, dy, dz, explosion_flinch_intensity(intensity, d2, range)});
    }
    return out;
}

std::vector<ExplosionFlinch> explosion_flinches(float ex, float ey, float ez, float range, float intensity,
                                                const PresenceWorld& world) {
    if (!(range > 0.0f)) return {};
    std::vector<ObjectId> ids;
    world.box_query(ex, ey, ez, range, ids);
    std::vector<FlinchCandidate> c;
    c.reserve(ids.size());
    for (const ObjectId id : ids) {
        UnitView u;
        if (!world.unit(id, u)) continue;
        c.push_back(FlinchCandidate{id, u.x, u.y, u.z, u.has_idle});
    }
    return explosion_flinches(ex, ey, ez, range, intensity, c);
}

}  // namespace alamo::tactical::presence
