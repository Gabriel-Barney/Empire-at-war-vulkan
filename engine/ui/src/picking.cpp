#include "alamo/ui/picking.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::ui {

namespace {
using tactical::cross;
using tactical::dot;
Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0, 0, 1};
}
}  // namespace

View make_view(const CameraParams& p, uint32_t width, uint32_t height) {
    View v;
    v.width = width ? width : 1;
    v.height = height ? height : 1;
    v.eye = p.eye;
    // Same basis as render::look_at_rh (world up +Z; fallback +Y when looking straight along Z).
    v.forward = normalize(p.target - p.eye);
    Vec3 s = cross(v.forward, p.up);  // world up +Z unless the camera is rolled / flipped [port: Gabriel]
    if (dot(s, s) < 1e-12f) s = cross(v.forward, Vec3{0, 1, 0});
    v.right = normalize(s);
    v.up = cross(v.right, v.forward);
    const float aspect = static_cast<float>(v.width) / static_cast<float>(v.height);
    v.tan_half_x = std::tan(0.5f * p.fov_x);
    v.tan_half_y = v.tan_half_x / aspect;  // fov_y = 2 atan(tan(fov_x / 2) / aspect)
    v.znear = p.znear;
    v.zfar = p.zfar;
    return v;
}

Vec3 view_dir(const View& v, float sx, float sy) {
    const float nx = 2.0f * sx / static_cast<float>(v.width) - 1.0f;
    const float ny = 1.0f - 2.0f * sy / static_cast<float>(v.height);
    const float vx = nx * v.tan_half_x, vy = ny * v.tan_half_y;
    return v.right * vx + v.up * vy + v.forward;
}

tactical::Ray screen_ray(const View& v, float sx, float sy) {
    return {v.eye, view_dir(v, sx, sy) * v.zfar, 1.0f};
}

bool project(const View& v, Vec3 p, float& sx, float& sy) {
    const Vec3 d = p - v.eye;
    const float z = dot(d, v.forward);
    if (!(z >= v.znear)) return false;
    const float nx = dot(d, v.right) / z / v.tan_half_x;
    const float ny = dot(d, v.up) / z / v.tan_half_y;
    sx = (nx + 1.0f) * 0.5f * static_cast<float>(v.width);
    sy = (1.0f - ny) * 0.5f * static_cast<float>(v.height);
    return true;
}

bool ray_plane_z(const tactical::Ray& r, float plane_z, Vec3& hit) {
    if (std::fabs(r.dir.z) < 1e-20f) return false;
    const float t = (plane_z - r.origin.z) / r.dir.z;
    if (!(t >= 0.0f) || t > r.max_t) return false;
    hit = r.origin + r.dir * t;
    hit.z = plane_z;
    return true;
}

std::optional<PickHit> pick(const tactical::SpatialTree& tree, const tactical::Ray& ray, tactical::QueryFilter filter,
                            tactical::QueryContext& ctx, uint32_t ignore) {
    tactical::RayHit h;
    if (!tree.raycast_first(ray, filter, ctx, h, ignore)) return std::nullopt;
    return PickHit{h.handle, h.t};
}

std::span<const tactical::RayHit> pick_all(const tactical::SpatialTree& tree, const tactical::Ray& ray,
                                           tactical::QueryFilter filter, tactical::QueryContext& ctx) {
    return tree.query_ray(ray, filter, ctx);
}

std::optional<size_t> pick_spheres(std::span<const PickSphere> items, const tactical::Ray& ray, float* t_out) {
    std::optional<size_t> best;
    float best_t = 0.0f;
    const float a = dot(ray.dir, ray.dir);
    if (!(a > 0.0f)) return best;
    for (size_t i = 0; i < items.size(); ++i) {
        const PickSphere& s = items[i];
        const Vec3 oc = ray.origin - s.center;
        const float c = dot(oc, oc) - s.radius * s.radius;
        float t = 0.0f;
        if (c > 0.0f) {
            const float b = dot(ray.dir, oc);
            const float disc = b * b - a * c;
            if (b > 0.0f || disc < 0.0f) continue;  // pointing away / missing
            t = (-b - std::sqrt(disc)) / a;
            if (t < 0.0f || t > ray.max_t) continue;
        }
        if (!best || t < best_t || (t == best_t && s.id < items[*best].id)) {
            best = i;
            best_t = t;
        }
    }
    if (best && t_out) *t_out = best_t;
    return best;
}

PixelBounds drag_select_bounds(const DragRect& rect) {
    return {static_cast<float>(rect.min_x()), static_cast<float>(rect.min_y()), static_cast<float>(rect.max_x() + 1),
            static_cast<float>(rect.max_y() + 1)};
}

std::vector<uint32_t> box_select(const tactical::SpatialTree& tree, const View& v, const DragRect& rect,
                                 tactical::QueryFilter filter, tactical::QueryContext& ctx) {
    const PixelBounds pb = drag_select_bounds(rect);
    const float x0 = pb.x0, x1 = pb.x1, y0 = pb.y0, y1 = pb.y1;
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    Vec3 nq[4], fq[4];
    for (int i = 0; i < 4; ++i) {
        const Vec3 d = view_dir(v, xs[i], ys[i]);
        nq[i] = v.eye + d * v.znear;
        fq[i] = v.eye + d * v.zfar;
    }
    const tactical::Frustum f = tactical::frustum_from_corners(nq, fq);
    std::vector<uint32_t> out;
    for (tactical::EntityHandle h : tree.query_frustum(f, filter, ctx)) {
        const tactical::Aabb* b = tree.bounds(h);
        if (!b) continue;
        const Vec3 c = tactical::center(*b);
        float sx = 0, sy = 0;
        if (!project(v, c, sx, sy)) continue;
        if (dot(c - v.eye, v.forward) > v.zfar) continue;
        if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) out.push_back(h);
    }
    return out;  // query_frustum is ascending handle already
}

}  // namespace alamo::ui
