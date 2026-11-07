// alamo_render - view frustum from a view-projection matrix and the box test shared by the model renderer's
// per-instance cull and the particle renderer's per-system cull (render only; never read by the simulation).
//
// Clip space is D3D-style (x, y in [-w, w], z in [0, w]; camera.hpp). A box is culled only when every corner lies
// outside one plane (the p-vertex test: the corner furthest along the plane normal is outside), which is the same
// result as transforming the 8 corners to clip space, up to float rounding at a plane boundary.
#pragma once
#include "alamo/assets/math.hpp"
#include "alamo/render/camera.hpp"

namespace alamo::render {

struct Frustum {
    float p[6][4];  // left, right, bottom, top, near, far; inside when dot(p.xyz, point) + p.w >= 0
};

inline Frustum frustum_of(const Mat4& vp) {
    Frustum f{};
    const auto& m = vp.m;
    auto set = [&](int i, float a, float b, float c, float d) {
        f.p[i][0] = a;
        f.p[i][1] = b;
        f.p[i][2] = c;
        f.p[i][3] = d;
    };
    for (int s = 0; s < 2; ++s) {
        const float sg = s == 0 ? 1.0f : -1.0f;
        set(s, m[3][0] + sg * m[0][0], m[3][1] + sg * m[0][1], m[3][2] + sg * m[0][2], m[3][3] + sg * m[0][3]);
        set(2 + s, m[3][0] + sg * m[1][0], m[3][1] + sg * m[1][1], m[3][2] + sg * m[1][2], m[3][3] + sg * m[1][3]);
    }
    set(4, m[2][0], m[2][1], m[2][2], m[2][3]);                                          // z >= 0
    set(5, m[3][0] - m[2][0], m[3][1] - m[2][1], m[3][2] - m[2][2], m[3][3] - m[2][3]);  // z <= w
    return f;
}

// False when the box lies entirely outside one plane.
inline bool aabb_visible(const Frustum& f, const assets::Aabb& b) {
    for (const auto& p : f.p) {
        const float x = p[0] >= 0 ? b.max.x : b.min.x, y = p[1] >= 0 ? b.max.y : b.min.y, z = p[2] >= 0 ? b.max.z : b.min.z;
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
    }
    return true;
}

}  // namespace alamo::render
