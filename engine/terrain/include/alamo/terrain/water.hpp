// alamo_terrain - water plane, water mesh and water shader inputs (TERRAIN_WATER_POST.md 3).
//
// One plane per map at header 15, drawn only when the type (header 16) is water/lava/ice.
// Geometry: for every terrain patch whose minimum height is below the plane, the patch's 31x31
// quads where any corner is below the plane or has a "not rendered" material; vertices at the
// terrain XY with Z = water height. Drawn in bucket 7 (Water) after the opaque/terrain work, one
// draw per water patch, identity world.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

// alD3dVertNC, 32 bytes (binding stride 32):
//   position offset  0 VK_FORMAT_R32G32B32_SFLOAT  (terrain x, y, water height)
//   normal   offset 12 VK_FORMAT_R32G32B32_SFLOAT  (0, 0, 1)
//   color0   offset 24 VK_FORMAT_B8G8R8A8_UNORM    white, alpha = shore factor (the water alpha)
//   color1   offset 28 VK_FORMAT_B8G8R8A8_UNORM    white
struct WaterVertex {
    float position[3];
    float normal[3];
    uint8_t color0[4];  // B, G, R, A
    uint8_t color1[4];
};
static_assert(sizeof(WaterVertex) == 32);
static_assert(offsetof(WaterVertex, color0) == 24 && offsetof(WaterVertex, color1) == 28);
constexpr uint32_t kWaterVertexStride = 32;

struct WaterPatch {
    uint32_t px = 0, py = 0;           // same patch grid as the terrain mesh
    std::vector<uint8_t> vertices;     // 32x32 WaterVertex (all patch vertices, row-major)
    std::vector<uint16_t> indices;     // triangle list over the included quads (same winding as terrain)
    assets::Aabb bounds;               // flat box at the water height over the patch
    uint32_t triangle_count() const { return static_cast<uint32_t>(indices.size() / 3); }
    const WaterVertex& vertex(uint32_t c, uint32_t r) const {
        return reinterpret_cast<const WaterVertex*>(vertices.data())[r * kPatchVerts + c];
    }
};

// Render targets the renderer must create for this surface (3.5). Formats are the D3D9 originals
// with their Vulkan equivalents.
struct WaterTargets {
    bool reflection = false;    // planar reflection RT (water t0 and ice): mirrored camera about
                                // z = height, clip plane (0,0,1) at height - clip_offset, pass flag 2
    bool refraction = false;    // refraction RT (water t0 only): same camera, clip (0,0,1) at
                                // height + clip_offset, pass flag 4
    bool sky_cube = false;      // 128^2 cube of the environment skydomes, cleared to the clear colour
    uint32_t size = 512;        // N x N, N = clamp(Water_Render_Target_Resolution, 256, 1024)
    uint32_t sky_cube_size = 128;
    const char* color_format = "VK_FORMAT_B8G8R8A8_UNORM";      // D3DFMT_A8R8G8B8
    const char* depth_format = "VK_FORMAT_D24_UNORM_S8_UINT";   // D3DFMT_D24S8 (D32_SFLOAT_S8_UINT if unsupported)
    float clip_plane_offset = 0.0f;  // GameConstants Water_Clip_Plane_Offset (vanilla 0.0)
};

struct WaterOptions {
    int32_t render_target_resolution = 512;  // GameConstants Water_Render_Target_Resolution
    float clip_plane_offset = 0.0f;          // GameConstants Water_Clip_Plane_Offset
};

// One texture binding of the DX9 technique: FXO parameter name -> texture file.
struct WaterTextureBinding {
    std::string parameter;
    std::string texture;   // file name (empty water names already replaced by w_generic_white.tga)
};

struct WaterSurface {
    WaterType type = WaterType::None;
    bool enabled = false;               // type != None and at least one water patch
    float height = 0.0f;
    float extent_min[2] = {0, 0}, extent_max[2] = {0, 0};  // the map's XY extent (plane size)
    std::string effect;                 // TerrainWater / TerrainLava / TerrainIce
    const char* technique_dx9 = "t0";   // WaterLOD 2 (t1 = DX8 WaterLOD 1, t2 = FF WaterLOD 0)
    bool alpha_blend = false;           // water: SRC_ALPHA / ONE_MINUS_SRC_ALPHA, z test + write;
                                        // lava / ice: opaque, z test + write (FXO)
    std::string textures[3];            // header 1D / 1E / 1F with the w_generic_white.tga fallback
    std::vector<WaterTextureBinding> bindings;  // t0 texture parameters (besides the RT / sky cube)
    WaterTargets targets;
    TerrainHeader header;               // copy of the terrain header (all water parameters)
    WaterScroll scroll[2];              // records 7 and 8
    std::vector<WaterPatch> patches;    // row-major patch order, only patches below the plane
    uint64_t triangles = 0, bytes = 0;
};

bool build_water(const Terrain& terrain, WaterSurface& out, const WaterOptions& options = {},
                 const ParallelFor& parallel = serial_for, std::string* error = nullptr);

// Shore factor of a vertex: clamp(m + (1-m) clamp((waterH - terrainH)/d, 0, 1), 0, 1).
float shore_alpha(float water_height, float terrain_height, float fade_depth, float min_alpha);

// Scroll record at time t with an extra direction offset (radians):
// U = (cos a / s, sin a / s, 0, v cos(d+off) t), V = (-0, cos a / s, 0, v sin(d+off) t).
void scroll_uv(const WaterScroll& s, float time, float direction_offset, std::array<float, 4>& u,
               std::array<float, 4>& v);

// Per-frame shader constants (3.3). time = water time (scene dt summed, each step clamped to 1 s).
struct WaterConstants {
    // TerrainWater t0
    std::array<float, 4> base_u{}, base_v{};      // BaseTexU/V: record 7
    std::array<float, 4> bump0_u{}, bump0_v{};    // Bump0TexU/V: record 7, direction +45 deg
    std::array<float, 4> bump1_u{}, bump1_v{};    // Bump1TexU/V: record 7, direction -45 deg
    std::array<float, 2> reflection_distortion{}; // ReflectionDistortionScale = h18 * 0.02 (both)
    std::array<float, 2> refraction_distortion{}; // RefractionDistortionScale = h19 * 0.02
    std::array<float, 4> reflect_c{}, refract_c{};// WaterReflectC = (colour r, 1), WaterRefractC = (colour (1-r), 1)
    std::array<float, 4> water_color{};           // WaterColor (DX8/FF) = (colour, header 20)
    std::array<float, 4> bump_env{};              // t1 BUMPENVMAT00/01/10/11: rotation by time*3.14,
                                                  // scaled by h18*0.02 ([L] element order)
    // m_texOffset is NOT set by the engine: keep the FXO default (no half-texel term).
    // TerrainLava t0
    std::array<float, 4> crust_u{}, crust_v{};    // CrustTexU/V: record 8 (Base = record 7)
    std::array<float, 4> crust_specular{};        // (colour, 1)
    // TerrainIce t0
    std::array<float, 4> ice_bump_u{}, ice_bump_v{};  // BumpTexU/V: record 8 (Base = record 7)
    std::array<float, 4> ice_diffuse{};           // (colour, 1)
    float ice_reflection_distortion = 0;          // h18 (not x0.02)
};
WaterConstants water_constants(const WaterSurface& surface, float time);

}  // namespace alamo::terrain
