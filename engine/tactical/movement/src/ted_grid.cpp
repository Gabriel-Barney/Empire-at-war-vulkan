#include "alamo/tactical/movement/ted_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "alamo/objects/game_object_types.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr objects::BehaviorTypeId kLandObstacle = 0x14;
constexpr objects::BehaviorTypeId kWall = 0x17;
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

uint32_t land_obstacle_footprint_type(const objects::GameObjectTypes& types, uint32_t t) {
    if (t >= types.size()) return t;
    const objects::TypeIndex proxy = types.get_type_ref(t, "Obstacle_Proxy_Type");
    return proxy < types.size() ? proxy : t;
}

uint32_t land_obstacle_bits(uint32_t override_bits, bool land_obstacle, bool wall) {
    // S8 t13 (MOVEMENT §1.3 "proxy bits", 0x397350 / 0x4c36b0): each of the object's OWN behaviours yields the type's
    // OverridePassability when non-zero, else its default (LAND_OBSTACLE Obstacle 4, WALL EmpireWall 0x10, others 0).
    if (override_bits != 0) return override_bits & 0x1FFu;
    return (land_obstacle ? kPassObstacle : 0u) | (wall ? kPassEmpireWall : 0u);
}

bool land_obstacle_footprint(const objects::GameObjectTypes& types, uint32_t t, V2 position, float yaw_deg, float terrain_z,
                             const MapStampHooks* hooks, ObstacleFootprint& out) {
    if (t >= types.size()) return false;
    // S8 t09 (MOVEMENT §1.3): Obstacle_Proxy_Type replaces the type for the sizes, offsets, model box and Fine_Tune byte.
    const uint32_t f = land_obstacle_footprint_type(types, t);
    const bool proxied = f != t;
    const float w = types.get_float(f, "Obstacle_Width"), hgt = types.get_float(f, "Obstacle_Height");
    const float xo = types.get_float(f, "Obstacle_X_Offset"), yo = types.get_float(f, "Obstacle_Y_Offset");
    const float scale = types.get_float(f, "Scale_Factor") > 0.0f ? types.get_float(f, "Scale_Factor") : 1.0f;
    const bool fine = types.get_bool(f, "Fine_Tune_Occupied_Passability");
    const StampRayModel rm = hooks && hooks->ray_model ? hooks->ray_model(f) : StampRayModel::Unknown;
    // S8 t13 (§1.3 "A type without Land_Model_Name"): fine-tune on, no proxy, no land model -> the stamper returns before
    // its loop and "stamped" stays clear (retried every service; the port's model data is const: never stamped).
    if (fine && rm == StampRayModel::None && !proxied) return false;
    // OI tactical: model-bbox footprint (§1.3). S8 t13: each size getter (0x372d80 ... 0x372f60) uses its XML value when it
    // is non-zero, else the model box (0 without a model).
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    const bool have_model = hooks && hooks->model_half_extents && hooks->model_half_extents(f, hx, hy, hz);
    if (!have_model) {
        hx = hy = hz = 0.0f;
        // [port] no model data in this run (Unknown): without both XML sizes there is no footprint (NoFootprint). A type
        // known to have no land model (None: fine-tune off, or a proxy) gets its explicit rectangle or zero samples.
        if (rm != StampRayModel::None && (w == 0.0f || hgt == 0.0f)) return false;
    }
    const float ax = std::fabs(hx) * scale, ay = std::fabs(hy) * scale;
    // Fine_Tune_Occupied_Passability: the ray model decides (header).
    std::function<bool(V2)> fine_tune;
    if (fine && rm == StampRayModel::Model && hooks->mesh_ray_hit) {
        StampPose pose;
        pose.position = position;
        pose.z = terrain_z;
        pose.yaw_deg = yaw_deg;
        pose.scale = scale;
        fine_tune = [ray = hooks->mesh_ray_hit, f, pose](V2 xy) { return ray(f, pose, xy); };
    }
    out = ObstacleFootprint{};
    out.position = position;
    out.yaw_deg = yaw_deg;
    out.width = w != 0.0f ? w : 2.0f * ax;
    out.height = hgt != 0.0f ? hgt : 2.0f * ay;
    out.x_offset = xo != 0.0f ? xo : -ax;
    out.y_offset = yo != 0.0f ? yo : -ay;
    // The bits come from the object's own type / behaviours, never from the proxy (hosts with live behaviour lists
    // recompute them with land_obstacle_bits).
    out.bits = land_obstacle_bits(static_cast<uint32_t>(types.get_mask(t, "OverridePassability")), true,
                                  types.has_behavior(t, objects::BehaviorSet::Union, kWall));
    out.terrain_z = terrain_z;
    // Vertical height = model bbox z x scale; unknown model height: treat as tall (always stamps).
    out.obstacle_height = have_model ? std::fabs(hz) * scale : 1.0e6f;
    out.fine_tune = std::move(fine_tune);
    return true;
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
        ObstacleFootprint fp;
        if (!land_obstacle_footprint(types, t, {o.position[0], o.position[1]}, o.rotation_deg[2], o.position[2], hooks, fp)) continue;
        if (grid.stamp(map_stamp_id(i), fp) > 0) ++n;
    }
    return n;
}

V2 space_obstacle_center(const objects::GameObjectTypes& types, uint32_t t, V2 position, V2 f) {
    // S7 t07: + Space_Obstacle_Offset, x along the facing, y to its left [M: rotation sense].
    if (t >= types.size()) return position;
    const std::array<float, 4> off = types.get_vec(t, "Space_Obstacle_Offset");
    return {position.x + off[0] * f.x - off[1] * f.y, position.y + off[0] * f.y + off[1] * f.x};
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
        const V2 d = dir_from_deg(o.rotation_deg[2]);  // (cos, sin): the placement facing Rz(yaw) (0, -1) = (sin, -cos)
        occ.add_static(map_stamp_id(i), space_obstacle_center(*types, it->second, {o.position[0], o.position[1]}, {d.y, -d.x}), r, bits);
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
