#include "alamo/tactical/movement/ted_grid.hpp"

#include <unordered_map>

#include "alamo/objects/game_object_types.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr objects::BehaviorTypeId kLandObstacle = 0x14;
constexpr objects::BehaviorTypeId kSpaceObstacle = 0x4C;

// type CRC (name_crc of the upper-cased name) -> first definition. Lookup only.
std::unordered_map<uint32_t, uint32_t> crc_index(const objects::GameObjectTypes& types) {
    std::unordered_map<uint32_t, uint32_t> m;
    const auto& tc = types.raw().types();
    for (uint32_t t = 0; t < tc.size(); ++t) m.emplace(tc.name_crc[t], t);
    return m;
}

}  // namespace

PassabilityGrid grid_from_map(const terrain::MapData& map) {
    PassabilityGrid g;
    const uint32_t w = map.terrain.width, h = map.terrain.height;
    g.init(w, h);
    if (w == 0 || h == 0) return g;
    if (map.passability_present && map.passability.size() == static_cast<size_t>(w) * h) {
        g.set_base(map.passability);
    } else if (map.legacy_passability.size() == static_cast<size_t>(w) * h) {
        // Legacy 0x104: one byte per cell, column-major like 0x10A [M].
        std::vector<uint32_t> v(static_cast<size_t>(w) * h, kPassClear);
        for (uint32_t x = 0; x < w; ++x) {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t b = map.legacy_passability[static_cast<size_t>(x) * h + y];
                v[static_cast<size_t>(y) * w + x] = b == 1 ? kPassSteep : (b == 2 ? kPassObstacle : kPassClear);
            }
        }
        g.set_base(v);
    }
    float mn[2], mx[2];
    if (map.playable.present && map.playable.has_box && map.playable_box(mn, mx)) g.set_playable_box({mn[0], mn[1]}, {mx[0], mx[1]});
    if (map.terrain.header.water_type != terrain::WaterType::None) g.set_water_height(map.terrain.header.water_height);
    return g;
}

uint32_t stamp_map_obstacles(PassabilityGrid& grid, const terrain::MapData& map, const objects::GameObjectTypes& types,
                             const MapStampHooks* hooks) {
    const auto idx = crc_index(types);
    uint32_t n = 0;
    for (size_t i = 0; i < map.objects.size(); ++i) {
        const terrain::ObjectPlacement& o = map.objects[i];
        auto it = idx.find(o.type_crc);
        if (it == idx.end()) continue;
        const uint32_t t = it->second;
        if (!types.has_behavior(t, objects::BehaviorSet::Union, kLandObstacle)) continue;
        const float w = types.get_float(t, "Obstacle_Width"), hgt = types.get_float(t, "Obstacle_Height");
        const float scale = types.get_float(t, "Scale_Factor") > 0.0f ? types.get_float(t, "Scale_Factor") : 1.0f;
        // OI tactical: model-bbox footprint when the XML sizes are not set (§1.3).
        float hx = 0.0f, hy = 0.0f, hz = 0.0f;
        const bool have_model = hooks && hooks->model_half_extents && hooks->model_half_extents(t, hx, hy, hz);
        if ((!(w > 0.0f) || !(hgt > 0.0f)) && !(have_model && (w > 0.0f || std::fabs(hx) > 0.0f) && (hgt > 0.0f || std::fabs(hy) > 0.0f)))
            continue;
        const uint32_t bits = static_cast<uint32_t>(types.get_mask(t, "OverridePassability"));
        ObstacleFootprint fp = make_footprint({o.position[0], o.position[1]}, o.rotation_deg[2], w, hgt,
                                              types.get_float(t, "Obstacle_X_Offset"), types.get_float(t, "Obstacle_Y_Offset"),
                                              have_model ? hx : 0.0f, have_model ? hy : 0.0f, scale, bits);
        fp.terrain_z = o.position[2];
        // Vertical height = model bbox z x scale; unknown model height: treat as tall (always stamps).
        fp.obstacle_height = have_model ? std::fabs(hz) * scale : 1.0e6f;
        if (hooks && hooks->mesh_ray_hit && types.get_bool(t, "Fine_Tune_Occupied_Passability")) {
            const auto* mr = &hooks->mesh_ray_hit;
            fp.fine_tune = [mr, i](V2 xy) { return (*mr)(i, xy); };
        }
        if (grid.stamp(map_stamp_id(i), fp) > 0) ++n;
    }
    return n;
}

uint32_t space_from_map(SpaceOccupancy& occ, const terrain::MapData& map, const objects::GameObjectTypes* types,
                        const SpaceObstacleHooks* hooks) {
    float mn[2], mx[2];
    if (map.playable_box(mn, mx)) occ.set_box({mn[0], mn[1]}, {mx[0], mx[1]});
    if (!types) return 0;
    const auto idx = crc_index(*types);
    uint32_t n = 0;
    for (size_t i = 0; i < map.objects.size(); ++i) {
        const terrain::ObjectPlacement& o = map.objects[i];
        auto it = idx.find(o.type_crc);
        if (it == idx.end()) continue;
        float r = 0.0f;
        uint32_t bits = 0;
        if (!space_obstacle_footprint(*types, it->second, r, bits, hooks)) continue;
        // S7 t07: + Space_Obstacle_Offset, x along the placement facing Rz(yaw) (0, -1), y to its left [M: rotation sense].
        const std::array<float, 4> off = types->get_vec(it->second, "Space_Obstacle_Offset");
        const V2 d = dir_from_deg(o.rotation_deg[2]);  // (cos, sin): facing = (sin, -cos), left = (cos, sin)
        const V2 f{d.y, -d.x};
        occ.add_static(-static_cast<ObjectId>(i) - 1,
                       {o.position[0] + off[0] * f.x - off[1] * f.y, o.position[1] + off[0] * f.y + off[1] * f.x}, r, bits);
        ++n;
    }
    return n;
}

bool space_obstacle_footprint(const objects::GameObjectTypes& types, uint32_t t, float& radius, uint32_t& bits,
                              const SpaceObstacleHooks* hooks) {
    if (t >= types.size() || !types.has_behavior(t, objects::BehaviorSet::Union, kSpaceObstacle)) return false;
    float r = types.get_float(t, "Custom_Soft_Footprint_Radius");
    if (!(r > 0.0f)) r = types.get_float(t, "Space_Obstacle_Radius");
    if (!(r > 0.0f)) {
        // S7 t07 (MOVEMENT §2.2, getter 0x373b50): the larger x / y half extent of the space model's collision bound.
        // [port] no model data: no entry (retail: radius 0 without a model).
        float hx = 0.0f, hy = 0.0f;
        if (!hooks || !hooks->model_half_extents || !hooks->model_half_extents(t, hx, hy)) return false;
        r = std::max(std::fabs(hx), std::fabs(hy));
        if (!(r > 0.0f)) return false;
    }
    const float scale = types.get_float(t, "Scale_Factor") > 0.0f ? types.get_float(t, "Scale_Factor") : 1.0f;
    radius = r * scale;
    bits = kSpaceStatic;
    if (types.get_bool(t, "Is_Impassable_Asteroid")) bits |= kSpaceImpassableAsteroid;
    return true;
}

}  // namespace alamo::tactical::movement
