// alamo_render - procedural laser projectiles (docs/re/PROJECTILE_NEBULA_DRAW.md 1).
//
// Projectile types with Projectile_Custom_Render 1 or 2 never draw their ALO. The original builds a render object per
// projectile instead:
//   * 1 = beam (alLaserBeam): a camera-facing six-vertex "pill" on W_Laser_Pill.tga (4 x 4 atlas). Core segment A..B of
//     Projectile_Length centred on the projectile position along its flight axis; half-width and end caps
//     k = Projectile_Width x (1 + Laser_Beam_Z_Scale_Factor x (d - near) / (far - near)), d = view depth of the midpoint.
//   * 2 = kite (alLaserKite): a camera-facing four-vertex diamond on p_particle_master.tga (8 x 8 atlas), tinted by
//     Projectile_Laser_Color; half-width / tail k = W x (1 + Laser_Kite_Z_Scale_Factor x (d - near) / (far - near)), nose
//     k x max(1, L_perp) ahead of the position (L_perp = the length projected onto the view plane).
// Both draw in the Transparent bucket with Engine\PrimAdditive (ONE/ONE, no Z write, cull none, Z test LESSEQUAL), world-
// space vertices, all of a kind in one batch per frame. The renderer builds the vertices at draw time from the frame's
// camera (Renderer::set_lasers); the functions below are the CPU geometry (tests).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/render/camera.hpp"

namespace alamo::render {

enum class LaserKind : uint8_t { Beam = 1, Kite = 2 };

// GameConstants.xml Laser_Beam_Z_Scale_Factor / Laser_Kite_Z_Scale_Factor (vanilla 8.0 / 1.2).
struct LaserConstants {
    float beam_z_scale = 8.0f;
    float kite_z_scale = 1.2f;
};
// Reads the two factors from GameConstants.xml bytes (missing tags keep `out`'s values). False: not parseable.
bool parse_laser_constants(std::span<const uint8_t> xml, LaserConstants& out);

// One projectile of the frame. `position` = the projectile (render object origin), `direction` = its flight direction
// (the render object's local +Y, normalised), `length` = Projectile_Length x Scale_Factor, `width` = Projectile_Width.
struct Laser {
    LaserKind kind = LaserKind::Beam;
    Float3 position{0, 0, 0};
    Float3 direction{1, 0, 0};
    float width = 1.0f, length = 1.0f;
    uint8_t slot_col = 0, slot_row = 0;  // Projectile_Texture_Slot (column, row)
    uint8_t color[4] = {255, 255, 255, 255};  // Projectile_Laser_Color R, G, B, A (kites; beams are white)
};

// Engine PrimVert (XYZ | DIFFUSE | TEX1, 24 bytes; colour = D3DCOLOR, bytes B, G, R, A).
struct PrimVertex {
    float pos[3];
    uint32_t color;
    float uv[2];
};
static_assert(sizeof(PrimVertex) == 24, "PrimVert is 24 bytes");

// Index tables (beam: four triangles over its six vertices; kite: the quad (0,1,2) (0,2,3)).
inline constexpr uint32_t kLaserBeamIndices[12] = {0, 1, 2, 1, 2, 3, 2, 3, 4, 3, 4, 5};
inline constexpr uint32_t kLaserKiteIndices[6] = {0, 1, 2, 0, 2, 3};
inline constexpr const char* kLaserBeamTexture = "W_Laser_Pill.tga";
inline constexpr const char* kLaserKiteTexture = "p_particle_master.tga";

// Near / far of a perspective_rh projection (depth 0..1).
void projection_near_far(const Mat4& proj, float& znear, float& zfar);
// Distance factor f = 1 + z_scale x (d - near) / (far - near) at view depth d.
float laser_depth_factor(float d, float znear, float zfar, float z_scale);

// Geometry of one laser for the camera `view` (width / height = the viewport in pixels). False (nothing written): the
// laser is degenerate or behind the camera.
bool laser_beam_vertices(const Laser& l, const ViewMatrices& view, float z_scale, PrimVertex (&out)[6]);
bool laser_kite_vertices(const Laser& l, const ViewMatrices& view, float z_scale, PrimVertex (&out)[4]);

// All lasers of a frame as two batches (beams, kites) in input order.
struct LaserGeometry {
    std::vector<PrimVertex> beam_vertices, kite_vertices;
    std::vector<uint32_t> beam_indices, kite_indices;
    uint32_t beams = 0, kites = 0, skipped = 0;
    void build(std::span<const Laser> lasers, const ViewMatrices& view, const LaserConstants& k);
};

}  // namespace alamo::render
