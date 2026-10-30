#include "alamo/terrain/placement.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "alamo/core/log.hpp"

namespace alamo::terrain {

// Render-only: std::cos/sin are not correctly rounded and may differ between CRTs; this matrix
// places models for drawing. Sim code uses the stored position / rotation fields, not this.
assets::Mat3x4 placement_transform(const float (&p)[3], const float (&r)[3], float s) {
    constexpr float k = 0.017453292519943295f;
    const float cx = std::cos(r[0] * k), sx = std::sin(r[0] * k);
    const float cy = std::cos(r[1] * k), sy = std::sin(r[1] * k);
    const float cz = std::cos(r[2] * k), sz = std::sin(r[2] * k);
    // R = Rz * Ry * Rx (column vectors).
    const float m[3][3] = {
        {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
        {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
        {-sy, cy * sx, cy * cx},
    };
    assets::Mat3x4 out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out.m[i][j] = m[i][j] * s;
        out.m[i][3] = p[i];
    }
    return out;
}

PlacementResult resolve_placements(const MapData& map, const TypeResolver& resolver, const PlacementOptions& opt) {
    PlacementResult res;
    std::map<uint32_t, uint32_t> unknown;
    std::map<uint32_t, std::pair<bool, ResolvedType>> cache;  // CRC -> (known, type)
    const bool space = map.is_space();
    for (uint32_t i = 0; i < map.objects.size(); ++i) {
        const ObjectPlacement& o = map.objects[i];
        auto it = cache.find(o.type_crc);
        if (it == cache.end()) {
            ResolvedType t;
            const bool known = resolver && resolver(o.type_crc, space, t);
            it = cache.emplace(o.type_crc, std::make_pair(known, std::move(t))).first;
        }
        const auto& [known, type] = it->second;
        if (!known) {
            res.unknown_type.push_back(i);
            ++unknown[o.type_crc];
            continue;
        }
        if (type.model_name.empty()) {
            res.no_model.push_back(i);
            if (!opt.keep_without_model) continue;
        }
        ResolvedPlacement p;
        p.object_index = i;
        p.type_crc = o.type_crc;
        p.type_name = type.type_name;
        p.model_name = type.model_name;
        p.owner = o.owner;
        p.scale = type.scale_factor;
        p.initial_health_fraction = o.health_fraction;
        if (!std::isfinite(p.initial_health_fraction)) {
            log::warnf("terrain", "object %u (type %s): non-finite initial health fraction; using 1.0", i,
                       type.type_name.c_str());
            p.initial_health_fraction = 1.0f;
        }
        std::copy(o.position, o.position + 3, p.position);
        if (opt.snap_to_terrain && map.terrain.present) p.position[2] = map.terrain.sample_height(p.position[0], p.position[1]);
        p.transform = placement_transform(p.position, o.rotation_deg, p.scale);
        res.placements.push_back(std::move(p));
    }
    res.unknown_crcs.assign(unknown.begin(), unknown.end());
    return res;
}

}  // namespace alamo::terrain
