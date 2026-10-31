#include "alamo/ai/tactical/perception_lua.hpp"

#include <algorithm>
#include <vector>

#include "alamo/core/dmath.hpp"

namespace alamo::ai::tactical {

DefendedPosition most_defended_position_cells(const TacticalPerceptionGrid& g, const PerceptionWorld& w,
                                              std::span<const uint32_t> cells, int32_t player, SimRng& rng,
                                              bool memory_counted_when_not_fogged) {
    DefendedPosition r;
    if (!g.valid() || (g.category_union() & ~0ull) == 0) return r;  // §17: grid mask & category mask 0 -> 1e18, no draws
    GridQuery q;
    q.category = ~0ull;
    q.self = player;
    q.ally = true;
    q.attenuator = 0.0f;
    q.memory_counted_when_not_fogged = memory_counted_when_not_fogged;
    float best = -1.0f;  // §17: the first cell is always taken (no draw); every later tie draws, 0 == 0 included
    for (const uint32_t cell : cells) {
        if (cell >= g.cell_count()) continue;
        const float s = g.cell_force(w, cell, q);
        if (s > best) {
            best = s;
            r.cell = static_cast<int32_t>(cell);
        } else if (s == best) {
            ++r.draws;
            if (rng.inline_unit_float() > 0.5f) r.cell = static_cast<int32_t>(cell);
        }
    }
    if (r.cell >= 0) {
        const auto nx = static_cast<uint32_t>(g.nx());
        r.position = g.cell_center(static_cast<int32_t>(static_cast<uint32_t>(r.cell) % nx),
                                   static_cast<int32_t>(static_cast<uint32_t>(r.cell) / nx));
    }
    return r;
}

DefendedPosition most_defended_position_space(const TacticalPerceptionGrid& g, const PerceptionWorld& w, const Vec3f& pos,
                                              float region_size, int32_t player, SimRng& rng) {
    std::vector<uint32_t> cells;
    const float h = region_size * 0.5f;
    g.rect_cells({pos.x - h, pos.y - h, pos.x + h, pos.y + h}, cells);
    return most_defended_position_cells(g, w, cells, player, rng);
}

float self_threat_max(std::span<const SelfThreatUnit> units) {
    float m = 0.0f;
    bool any = false;
    for (const SelfThreatUnit& u : units) {
        if (!u.alive || !u.has_type) continue;
        if (!any || u.combat_power > m) m = u.combat_power;
        any = true;
    }
    return m;
}

float self_threat_sum(std::span<const SelfThreatUnit> units) {
    float s = 0.0f;
    for (const SelfThreatUnit& u : units)
        if (u.alive && u.has_type) s += u.combat_power;
    return s;
}

float unit_projection_range(const PerceptionWorld& w, const PerceptionObject& unit) {
    const PerceptionObject* src = &unit;
    if (unit.team_leader >= 0) {
        if (const PerceptionObject* leader = w.object(unit.team_leader)) src = leader;
    }
    float r = src->max_attack_range;
    std::vector<PerceptionHardPoint> hps;
    w.object_hard_points(unit, hps);
    for (const PerceptionHardPoint& hp : hps) {
        if (hp.type < kFirstGridHardPointType || hp.type > kLastGridHardPointType) continue;
        if (hp.destroyed) continue;
        r = std::max(r, hp.weapon_range);
    }
    return r;
}

std::optional<Vec3f> project_by_unit_range(const Vec3f& unit_pos, float range, const Vec3f& pos, const Rect2& map) {
    const float dx = pos.x - unit_pos.x, dy = pos.y - unit_pos.y, dz = pos.z - unit_pos.z;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return std::nullopt;
    const float inv = dmath::fast_rsqrt_retail(dx * dx + dy * dy + dz * dz);
    Vec3f out{pos.x + dx * inv * range, pos.y + dy * inv * range, pos.z + dz * inv * range};
    out.x = std::clamp(out.x, map.min_x, map.max_x);
    out.y = std::clamp(out.y, map.min_y, map.max_y);
    return out;
}

std::optional<Vec3f> project_by_unit_range(const PerceptionWorld& w, const PerceptionObject& unit, const Vec3f& pos) {
    return project_by_unit_range(unit.position, unit_projection_range(w, unit), pos, w.map);
}

std::optional<Vec3f> project_by_unit_range(const PerceptionWorld& w, const PerceptionObject* unit, const Vec3f& unit_pos, const Vec3f& pos) {
    return unit ? project_by_unit_range(w, *unit, pos) : project_by_unit_range(unit_pos, 0.0f, pos, w.map);
}

bool is_good_against(const galactic::ContrastTable& contrast, std::string_view caller_type_name, uint64_t caller_categories,
                     uint64_t target_categories) {
    bool any = false;
    float best = 0.0f;
    for (uint32_t b = 0; b < 64; ++b) {
        const uint64_t bit = uint64_t{1} << b;
        if (!(target_categories & bit)) continue;
        for (size_t j = 0; j < contrast.size(); ++j) {
            if (!(contrast.rows[j].enemy_mask & bit)) continue;
            const float v = contrast.contrast(j, caller_type_name, caller_categories);
            if (!any || v > best) best = v;
            any = true;
        }
    }
    return any && best > 1.0f;
}

}  // namespace alamo::ai::tactical
