// refixf t08: host side of the tactical world pick (HUD_GAPS §2.2). See world_pick.hpp.
#include "alamo/game/world_pick.hpp"

#ifdef ALAMO_GAME_HAVE_UI
#include <cmath>

#include "alamo/sim/game_modes.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"

namespace alamo::game {

namespace mv = tactical::movement;

std::vector<float> mouse_sphere_radii(const objects::GameObjectTypes& types) {
    std::vector<float> r(types.size(), 0.0f);
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        const float v = types.get_float(t, "Mouse_Collide_Override_Sphere_Radius");
        r[t] = (v > 0.0f && std::isfinite(v)) ? v : 0.0f;
    }
    return r;
}

ui::WorldPickTests tactical_pick_tests(const sim::TacticalMode& mode, const sim::TacticalBattle* battle,
                                       std::span<const float> radii) {
    ui::WorldPickTests t;
    const sim::TacticalMode* tm = &mode;
    t.precise = [tm](uint32_t id, const tactical::Ray& ray, float& out) {
        const objects::ObjectStore& st = tm->primary();
        const objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(id));
        if (!st.exists(h)) return ui::PreciseOutcome::NotModelled;
        mv::AvoidObstacle o;
        if (!tm->obstacle_of(h, o, false)) return ui::PreciseOutcome::NotModelled;  // const read: no cache fill
        const tactical::Vec3 e = ray.origin + ray.dir * ray.max_t;
        const mv::F3 a{ray.origin.x, ray.origin.y, ray.origin.z}, b{e.x, e.y, e.z};
        mv::AvoidSegmentHit hit;
        // [port] query mask 1 (hull meshes); a shielded type's SHIELD mesh gets mask 0 here (never part of the pick).
        if (!mv::avoid_precise_segment(o, a, b, 1u, 0u, hit)) return ui::PreciseOutcome::Miss;
        out = hit.t * ray.max_t;
        return ui::PreciseOutcome::Hit;
    };
    t.sphere = [tm, radii](uint32_t id, tactical::Vec3& centre) {
        const objects::ObjectStore& st = tm->primary();
        const objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(id));
        if (!st.exists(h)) return 0.0f;
        const objects::TypeIndex ty = st.type(h);
        if (ty >= radii.size() || !(radii[ty] > 0.0f)) return 0.0f;
        const auto p = st.position(h);
        centre = {p.x, p.y, p.z};
        return radii[ty];
    };
    if (battle) {
        t.skip = [battle](uint32_t id) {
            return battle->contained_by(static_cast<objects::ObjectId>(id)) != objects::kNoObjectId;
        };
    }
    return t;
}

}  // namespace alamo::game
#endif
