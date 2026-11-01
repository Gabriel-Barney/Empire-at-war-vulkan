// Space avoidance tree (GOM +0x558) and the precise model test 0x38f900 (docs/re/SPACE_AVOIDANCE.md), the rules behind
// the fighter locomotor's obstacle-avoidance hook (FighterAvoidFn, fighter.hpp; FIGHTER_FLYING §3.5).
//
// Pure geometry on host-supplied data: the host builds one AvoidModel per type (from the ALO collision meshes, model
// space, bind pose) and, per tick, the member list (AvoidObstacle, ascending object id, committed transforms); the
// hook then calls space_avoid(). No assets dependency here.
//
// Object transform [port] (the battle host's object_to_world): world = P + s (m.x X + m.y Y + m.z Z) with the planar
// facing f = (fx, fy) normalised, X = (-fy, fx, 0) (model left), Y = (-fx, -fy, 0) (models face -Y), Z = up, s =
// Scale_Factor. No pitch / roll.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/tactical/movement/common.hpp"
#include "alamo/tactical/movement/fighter.hpp"

namespace alamo::tactical::movement {

// Model-space collision data of one type's model (§3.2, §5.3, §5.4).
struct AvoidModel {
    // Collision bound (alHModel +0x120): the bind-pose union of the collision-enabled meshes' boxes (shield bubble
    // included); centre 0, half 1 when the model has no collision mesh.
    F3 bound_c{0.0f, 0.0f, 0.0f}, bound_h{1.0f, 1.0f, 1.0f};
    // COLLISIONBOX sub-object (ModelClass +0x8c / +0x90): when present the precise test is the box path only.
    bool has_collision_box = false;
    F3 cbox_c, cbox_h;
    // Collision-enabled meshes in ALO order: model-space box (broad phase) and triangles (3 points each).
    struct Mesh {
        F3 lo, hi;
        bool shield = false;  // the `SHIELD` sub-object (skipped for types with the shield behaviour: mask 2 / 0)
        std::string name;     // ALO mesh name, upper case: the struck sub-object (TACTICAL_COMBAT §2.9 Q1 step 1)
        std::vector<F3> tris;
    };
    std::vector<Mesh> meshes;
};

// One member of the avoidance tree at query time.
struct AvoidObstacle {
    ObjectId id = -1;
    F3 pos;                   // P (obj +0x78), committed
    V2 facing{1.0f, 0.0f};    // planar facing (normalised by avoid_place)
    float scale = 1.0f;       // Scale_Factor
    const AvoidModel* model = nullptr;  // null: no model (box [P, P + 0.1], never hit)
    bool skip_shield = false; // the type has the shield behaviour: its SHIELD mesh is not tested (§5.6)
    bool collide = true;      // model-level collide switch (§5.5; off while contained) - gates the mesh path only
    F3 box_c, box_h;          // world AABB (§3.1), set by avoid_place
};

// §3.1: normalise the facing and compute the world AABB (no model: [P, P + 0.1]; else the collision bound through the
// object transform, OBB -> AABB).
void avoid_place(AvoidObstacle& o);
// Model -> world / world -> model (the [port] transform above; inverse divides by s).
F3 avoid_to_world(const AvoidObstacle& o, F3 m);
F3 avoid_to_model(const AvoidObstacle& o, F3 w);

// 0x20c850 slab test of the segment o -> o + d against [lo, hi]: a start inside (inclusive, all axes) sets `inside`
// and returns true; else the largest per-axis plane fraction t (min plane when o < min, max when o > max; first
// maximum), a miss when t < 0 or the point on the other axes leaves [lo, hi] (inclusive), and a hit only when
// t < t_best.
bool avoid_segment_box(F3 o, F3 d, F3 lo, F3 hi, float t_best, bool& inside);
// 0x20c410 two-sided segment-triangle test: |det| >= 1e-4, u in [0, 1], v >= 0, u + v <= 1, 0 < t < t_best.
bool avoid_segment_triangle(F3 o, F3 d, F3 a, F3 b, F3 c, float t_best);
// §5.2 precise test 0x38f900 of the segment a -> b (world) against one obstacle. `inside` is the hit record's byte 1
// (shared across candidates, never reset). No model: false. COLLISIONBOX: box path. Else (when `collide`) the
// collision-enabled meshes (skip_shield drops the SHIELD mesh): broad phase vs the mesh box, then its triangles.
bool avoid_precise(const AvoidObstacle& o, F3 a, F3 b, bool& inside);
// avoid 0x5c8b70 steps 2-4 (§6): the members whose world AABB overlaps (inclusive) the query box (centre, half), in
// the given order (the host passes ascending object id [port]); the first whose precise test hits decides: byte 1
// clear -> true with its position in hit_pos; byte 1 set -> skipped, and every later hit is rejected (false).
bool space_avoid(std::span<const AvoidObstacle> members, F3 centre, F3 half, F3 a, F3 b, F3& hit_pos);

// refixf t07: the precise test 0x38f900 as the projectile segment query calls it (0x2abcf0, TACTICAL_COMBAT §3.4 /
// §2.9 Q1). Query mask `mask` (7 with Projectile_Does_Shield_Damage, else 5); mesh masks (§5.6) are 1, except the
// SHIELD mesh of a type with the shield behaviour (o.skip_shield), whose mask is `shield_mask` (2 while shield points
// > 0, else 0). One hit record over the whole model (t starts at 1.0, a hit needs 0 < t < record.t), so the surviving
// hit is the nearest; `mesh` is its index in AvoidModel::meshes (first in ALO order on equal t). No model: false.
// COLLISIONBOX: the slab test only, mesh -1 (no sub-object); a start inside is a hit at t 0 [port: retail latches
// byte 1, §5.7; only EM11_HYPERVADER has the box]. Else, when o.collide (§5.5), per mesh whose mask meets the query:
// broad phase vs the mesh box (private record), then its triangles.
struct AvoidSegmentHit {
    float t = 1.0f;
    int32_t mesh = -1;
};
bool avoid_precise_segment(const AvoidObstacle& o, F3 a, F3 b, uint32_t mask, uint32_t shield_mask, AvoidSegmentHit& hit);

}  // namespace alamo::tactical::movement
