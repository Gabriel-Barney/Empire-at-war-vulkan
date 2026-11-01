// P1 picking: screen rays from CameraParams (same conventions as render/camera.hpp: RH, Z up, horizontal
// FOV, look_at with world up +Z, clip-space Y up), ray picks against the tactical spatial trees, galactic
// sphere picks and box selection.
//
// Order: every pick result is sorted by (distance along the ray, object id) - the S6 tree's query_ray
// order (t_enter, handle); box selection returns ascending object id. Ids are gameplay object ids (the
// tree key), which are the commands' wire ids (OBJECT_RUNTIME id space).
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "alamo/tactical/spatial_tree.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/input.hpp"

namespace alamo::ui {

// Camera basis + projection for one viewport.
struct View {
    Vec3 eye;
    Vec3 right, up, forward;  // world-space camera axes (forward = towards the target)
    float tan_half_x = 1, tan_half_y = 1;
    float znear = 10, zfar = 7000;
    uint32_t width = 1, height = 1;
};
View make_view(const CameraParams& p, uint32_t width, uint32_t height);

// World direction through a screen point at view depth 1 (forward component exactly 1). Screen
// coordinates are pixels from the top-left; the centre of pixel (i, j) is (i + 0.5, j + 0.5).
Vec3 view_dir(const View& v, float sx, float sy);
// Pick ray from the eye to the far plane: dir = view_dir * zfar, max_t = 1 (t = view depth / zfar).
tactical::Ray screen_ray(const View& v, float sx, float sy);
inline tactical::Ray pixel_ray(const View& v, int32_t px, int32_t py) {
    return screen_ray(v, static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f);
}
// World point -> screen pixels; false when the point is not in front of the near plane.
bool project(const View& v, Vec3 p, float& sx, float& sy);
// Intersection with the horizontal plane z = plane_z (t >= 0, within max_t).
bool ray_plane_z(const tactical::Ray& r, float plane_z, Vec3& hit);

struct PickHit {
    uint32_t id = tactical::kNoEntity;
    float t = 0.0f;  // ray parameter (view depth / zfar)
};

// First hit by (t, id), skipping `ignore`; nullopt when nothing is hit.
std::optional<PickHit> pick(const tactical::SpatialTree& tree, const tactical::Ray& ray, tactical::QueryFilter filter,
                            tactical::QueryContext& ctx, uint32_t ignore = tactical::kNoEntity);
// All hits in (t, id) order (valid until ctx is reused).
std::span<const tactical::RayHit> pick_all(const tactical::SpatialTree& tree, const tactical::Ray& ray,
                                           tactical::QueryFilter filter, tactical::QueryContext& ctx);

// refixf t08: the tactical world pick 0x36600 (docs/re/HUD_GAPS.md §2.2). Per candidate: the precise model test (0x38f900,
// SPACE_AVOIDANCE §5); when it misses and the type's Mouse_Collide_Override_Sphere_Radius (type +0x4e0) is > 0, a
// ray-sphere test at the object centre with that radius. The nearest hit wins; a contained child resolves to its
// container. The tests are host callbacks (the model data lives in the sim host); all of them are const reads.
enum class PreciseOutcome : uint8_t {
    Miss = 0,
    Hit = 1,
    NotModelled = 2,  // no model data for the object: [port] its tree box stands in (the pre-t08 pick)
};
struct WorldPickTests {
    // Precise test of object `id` against the segment ray.origin -> ray.origin + ray.dir * ray.max_t. Hit: `t` = the hit
    // parameter on the ray (same units as Ray::max_t). Required: without it pick_world is the tree-box pick.
    std::function<PreciseOutcome(uint32_t id, const tactical::Ray& ray, float& t)> precise;
    // Mouse_Collide_Override_Sphere_Radius of the object's type (<= 0: none) and the sphere centre (the object position).
    std::function<float(uint32_t id, Vec3& centre)> sphere;
    // Candidates the pick skips (HUD_GAPS §2.2: garrisoned objects, flag +0x3a7). Optional.
    std::function<bool(uint32_t id)> skip;
    // The picked id for a hit candidate (a container child -> its container). Optional: identity.
    std::function<uint32_t(uint32_t id)> resolve;
};
// Ray-sphere entry parameter: origin inside -> 0 [port]; else the first intersection in [0, ray.max_t].
bool ray_sphere(const tactical::Ray& ray, Vec3 centre, float radius, float& t);
// The view frustum (near / far planes, full screen) used as the visible-set stand-in.
tactical::Frustum view_frustum(const View& v);
// The world pick. Candidates [port: the render scene's visible set]: the tree entries whose box meets the view frustum
// (`filter`, minus `ignore` and tests.skip), ascending id. Nearest hit by (t, resolved id) [port: equal t -> lower id].
// tests == nullptr or no tests->precise: pick() (tree boxes only).
std::optional<PickHit> pick_world(const tactical::SpatialTree& tree, const View& v, const tactical::Ray& ray,
                                  tactical::QueryFilter filter, tactical::QueryContext& ctx, const WorldPickTests* tests,
                                  uint32_t ignore = tactical::kNoEntity);

// Galactic map / any non-tree pickable: a sphere with an owner and a kind.
enum class PickKind : uint8_t { Object = 0, Fleet = 1, Planet = 2 };
struct PickSphere {
    uint32_t id = 0;
    Vec3 center;
    float radius = 1.0f;
    int32_t owner = -1;
    PickKind kind = PickKind::Object;
};
// Nearest sphere entry point by (t, id) (origin inside a sphere: t = 0); returns the index into `items`.
std::optional<size_t> pick_spheres(std::span<const PickSphere> items, const tactical::Ray& ray, float* t_out = nullptr);

// Screen-space bounds of a drag rectangle in pixel-edge coordinates (top-left origin, +Y down): the press and cursor
// pixels are both inside, i.e. [min_x, max_x + 1] x [min_y, max_y + 1]. box_select tests against these bounds and the
// tactical HUD draws the drag-select box (add_drag_box) over exactly the same area.
struct PixelBounds {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
PixelBounds drag_select_bounds(const DragRect& rect);

// Objects whose box centre projects inside the pixel rectangle (inclusive pixels, any corner order, drag_select_bounds)
// and lies between the near and far planes. Candidates come from a frustum query of the rectangle's
// sub-frustum; result ascending id.
std::vector<uint32_t> box_select(const tactical::SpatialTree& tree, const View& v, const DragRect& rect,
                                 tactical::QueryFilter filter, tactical::QueryContext& ctx);

}  // namespace alamo::ui
