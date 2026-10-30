#include "alamo/sim/map_spawn.hpp"

#include "alamo/core/dmath.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::sim {

TypeCrcIndex::TypeCrcIndex(const objects::GameObjectTypes& types) {
    const auto& crc = types.raw().types().name_crc;
    const auto& name = types.raw().types().name;
    for (objects::TypeIndex t = 0; t < types.size(); ++t)
        if (!name[t].empty()) map_.emplace(crc[t], t);  // emplace keeps the FIRST definition
}

objects::TypeIndex TypeCrcIndex::find(uint32_t name_crc) const {
    const auto it = map_.find(name_crc);
    return it == map_.end() ? objects::kNone : it->second;
}

SpawnReport spawn_map(TacticalMode& mode, const terrain::MapData& map, const TypeCrcIndex& index,
                      const ParallelFor& pf) {
    SpawnReport r;
    float mn[2], mx[2];
    if (map.playable_box(mn, mx)) {
        float zmin = -1.0e6f, zmax = 1.0e6f;
        if (map.is_space()) {
            zmin = map.space.playable_min[2];
            zmax = map.space.playable_max[2];
        }
        mode.set_bounds({{mn[0], mn[1], zmin}, {mx[0], mx[1], zmax}});
    }
    std::map<uint32_t, uint32_t> unknown;
    for (const terrain::ObjectPlacement& p : map.objects) {
        ++r.placements;
        const objects::TypeIndex t = index.find(p.type_crc);
        if (t == objects::kNone) {
            ++r.unknown_type;
            ++unknown[p.type_crc];
            continue;
        }
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = static_cast<int32_t>(p.owner);
        sp.position = {p.position[0], p.position[1], p.position[2]};
        float s = 0.0f, c = 1.0f;
        dmath::sincos(p.rotation_deg[2] * (dmath::kPiF / 180.0f), s, c);
        sp.facing = {s, -c, 0.0f};  // Rz(yaw) x the model's nose (-Y, ALO convention): the placement as authored
        const ObjectHandle h = mode.spawn(sp, p.health_fraction);
        ++r.spawned;
        ++r.per_owner[sp.owner];
        r.total_health += mode.primary().health(h);
        const uint32_t cat = mode.category_of(h);
        if (cat == kCatUnit) ++r.units;
        else if (cat == kCatStructure) ++r.structures;
        else ++r.props;
    }
    mode.finish_load(pf);
    r.in_trees = mode.trees().size();
    r.unknown_crcs.assign(unknown.begin(), unknown.end());
    return r;
}

}  // namespace alamo::sim
