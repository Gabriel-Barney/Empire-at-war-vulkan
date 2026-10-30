// P1 picking: screen rays from CameraParams (same conventions as render/camera.hpp: RH, Z up, horizontal
// FOV, look_at with world up +Z, clip-space Y up), ray picks against the tactical spatial trees, galactic
// sphere picks and box selection.
//
// Order: every pick result is sorted by (distance along the ray, object id) - the S6 tree's query_ray
// order (t_enter, handle); box selection returns ascending object id. Ids are gameplay object ids (the
// tree key), which are the commands' wire ids (OBJECT_RUNTIME id space).
#pragma once
#include <cstdint>
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
