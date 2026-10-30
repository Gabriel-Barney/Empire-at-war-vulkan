// Space avoidance tree and precise model test (docs/re/SPACE_AVOIDANCE.md). See space_avoid.hpp.
#include "alamo/tactical/movement/space_avoid.hpp"

#include <cmath>

namespace alamo::tactical::movement {

namespace {
float avoid_dot(F3 a, F3 b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
F3 avoid_cross(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float avoid_axis(F3 v, int32_t i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
}  // namespace

void avoid_place(AvoidObstacle& o) {
    float fx = o.facing.x, fy = o.facing.y;
    const float len2 = fx * fx + fy * fy;
    if (!(len2 > 0.0f)) {
        fx = 1.0f;
        fy = 0.0f;
    } else {
        const float inv = 1.0f / std::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    }
    o.facing = {fx, fy};
    if (!o.model) {  // §3.1 case 2: [P, P + 0.1]
        const F3 q{o.pos.x + 0.1f, o.pos.y + 0.1f, o.pos.z + 0.1f};
        o.box_c = {(q.x + o.pos.x) * 0.5f, (q.y + o.pos.y) * 0.5f, (q.z + o.pos.z) * 0.5f};
        o.box_h = {std::fabs(q.x - o.pos.x) * 0.5f, std::fabs(q.y - o.pos.y) * 0.5f, std::fabs(q.z - o.pos.z) * 0.5f};
        return;
    }
    // §3.1 case 3: centre_i = M_i0 c.x + M_i1 c.y + M_i2 c.z + M_i3, half_i = |M_i0 h.x| + |M_i1 h.y| + |M_i2 h.z|.
    const float s = o.scale;
    const float m[3][3] = {{-fy * s, -fx * s, 0.0f}, {fx * s, -fy * s, 0.0f}, {0.0f, 0.0f, s}};
    const F3 c = o.model->bound_c, h = o.model->bound_h;
    const float p[3] = {o.pos.x, o.pos.y, o.pos.z};
    float bc[3], bh[3];
    for (int32_t i = 0; i < 3; ++i) {
        bc[i] = ((m[i][0] * c.x + m[i][1] * c.y) + m[i][2] * c.z) + p[i];
        bh[i] = (std::fabs(m[i][0] * h.x) + std::fabs(m[i][1] * h.y)) + std::fabs(m[i][2] * h.z);
    }
    o.box_c = {bc[0], bc[1], bc[2]};
    o.box_h = {bh[0], bh[1], bh[2]};
}

F3 avoid_to_world(const AvoidObstacle& o, F3 m) {
    const float s = o.scale, fx = o.facing.x, fy = o.facing.y;
    return {(-fy * s) * m.x + (-fx * s) * m.y + o.pos.x, (fx * s) * m.x + (-fy * s) * m.y + o.pos.y, s * m.z + o.pos.z};
}

F3 avoid_to_model(const AvoidObstacle& o, F3 w) {
    const float s = o.scale, fx = o.facing.x, fy = o.facing.y;
    const F3 d{w.x - o.pos.x, w.y - o.pos.y, w.z - o.pos.z};
    return {(-fy * d.x + fx * d.y) / s, (-fx * d.x - fy * d.y) / s, d.z / s};
}

bool avoid_segment_box(F3 o, F3 d, F3 lo, F3 hi, float t_best, bool& inside) {
    if (o.x >= lo.x && o.x <= hi.x && o.y >= lo.y && o.y <= hi.y && o.z >= lo.z && o.z <= hi.z) {
        inside = true;
        return true;
    }
    float t = -1.0f;
    int32_t axis = -1;
    for (int32_t i = 0; i < 3; ++i) {
        const float oi = avoid_axis(o, i), di = avoid_axis(d, i), l = avoid_axis(lo, i), hh = avoid_axis(hi, i);
        float ti = -1.0f;
        if (di != 0.0f) {
            if (oi < l) ti = (l - oi) / di;
            else if (oi > hh) ti = (hh - oi) / di;
        }
        if (axis < 0 || ti > t) {  // the largest t, first maximum
            t = ti;
            axis = i;
        }
    }
    if (t < 0.0f) return false;
    for (int32_t i = 0; i < 3; ++i) {
        if (i == axis) continue;
        const float q = avoid_axis(o, i) + avoid_axis(d, i) * t;
        if (q < avoid_axis(lo, i) || q > avoid_axis(hi, i)) return false;
    }
    return t < t_best;
}

bool avoid_segment_triangle(F3 o, F3 d, F3 a, F3 b, F3 c, float t_best) {
    const F3 e1 = f3_sub(b, a), e2 = f3_sub(c, a);
    const F3 pv = avoid_cross(d, e2);
    const float det = avoid_dot(e1, pv);
    if (!(std::fabs(det) >= 1e-4f)) return false;
    const float inv = 1.0f / det;
    const F3 tv = f3_sub(o, a);
    const float u = avoid_dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const F3 qv = avoid_cross(tv, e1);
    const float v = avoid_dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = avoid_dot(e2, qv) * inv;
    return t > 0.0f && t < t_best;
}

bool avoid_precise(const AvoidObstacle& o, F3 a, F3 b, bool& inside) {
    if (!o.model) return false;
    const AvoidModel& m = *o.model;
    const F3 ma = avoid_to_model(o, a), mb = avoid_to_model(o, b);
    const F3 d = f3_sub(mb, ma);
    if (m.has_collision_box) {  // §5.3: the box path only (the collide switch is not consulted)
        const F3 lo = f3_sub(m.cbox_c, m.cbox_h), hi = f3_add(m.cbox_c, m.cbox_h);
        return avoid_segment_box(ma, d, lo, hi, 1.0f, inside);
    }
    if (!o.collide) return false;  // §5.5
    for (const AvoidModel::Mesh& mesh : m.meshes) {  // ALO order; hits are ORed, any one decides the boolean
        if (mesh.shield && o.skip_shield) continue;
        bool private_inside = false;  // the broad phase's own record
        if (!avoid_segment_box(ma, d, mesh.lo, mesh.hi, 1.0f, private_inside)) continue;
        for (size_t i = 0; i + 2 < mesh.tris.size(); i += 3)
            if (avoid_segment_triangle(ma, d, mesh.tris[i], mesh.tris[i + 1], mesh.tris[i + 2], 1.0f)) return true;
    }
    return false;
}

bool space_avoid(std::span<const AvoidObstacle> members, F3 centre, F3 half, F3 a, F3 b, F3& hit_pos) {
    bool inside = false;  // hit-record byte 1: shared across candidates, never reset (§5.7)
    for (const AvoidObstacle& o : members) {
        // §4 overlap (0x20bb60): disjoint iff |c_q - c_l| > h_q + h_l on some axis.
        if (std::fabs(centre.x - o.box_c.x) > half.x + o.box_h.x || std::fabs(centre.y - o.box_c.y) > half.y + o.box_h.y ||
            std::fabs(centre.z - o.box_c.z) > half.z + o.box_h.z)
            continue;
        if (avoid_precise(o, a, b, inside) && !inside) {
            hit_pos = o.pos;
            return true;
        }
    }
    return false;
}

}  // namespace alamo::tactical::movement
