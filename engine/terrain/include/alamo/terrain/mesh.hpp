// alamo_terrain - terrain patch meshes + per-material splat passes (TERRAIN_WATER_POST.md 2).
//
// Output is GPU-ready CPU data (no Vulkan). Per patch (31x31 quads = 32x32 vertices, patch (i, j)
// covers vertices x in [31i, 31i+31], y in [31j, 31j+31], patches stored row-major):
//   * vertices : 1024 x TerrainVertex (36 bytes, D3D FVF XYZ|NORMAL|DIFFUSE|TEX1), row-major
//                (local index = row * 32 + column). World space; world matrix = identity.
//   * passes   : one per material used in the patch (ascending material index, "not rendered"
//                slots skipped), each with
//                  - a u16 triangle list of the quads touching the material (any corner has it),
//                    triangles (v, v+1, v+33), (v, v+33, v+32), v = row*32 + col;
//                  - a 32x32, 1-mip blend texture, raw D3D A4R4G4B4 texels, u16 little-endian
//                    (bits 15-12 A, 11-8 R, 7-4 G, 3-0 B). Texel (c, r) <-> local vertex (c, r);
//                    texels are only ever 0x0000 or 0xFFFF. Upload unchanged as
//                    VK_FORMAT_B4G4R4A4_UNORM_PACK16 with the rhi swizzle ARGB4_IN_BGRA4 (or any
//                    4444 format: since all four nibbles are equal, every channel order samples
//                    identically). Sampler: linear, clamp (BlendSampler in the FXO).
//   * blend UV : blendTexScale / blendTexOffset (float2): uv = world_xy * scale + offset maps the
//                patch extent to [1/64, 63/64] so texel centres land on vertices.
//   * bounds   : AABB of the patch vertices (cull per patch against the full-far frustum).
// Draw (per visible patch, row-major patch order, passes in order): alpha blend SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA, depth test LESS_OR_EQUAL + depth write, TerrainRenderBump `bump`.
// There is no LOD, no skirts and no stitching.
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

// 36-byte vertex. VkVertexInputAttributeDescription (binding stride 36):
//   position  offset  0  VK_FORMAT_R32G32B32_SFLOAT  world (20x, 20y, h * 0.048828125)
//   normal    offset 12  VK_FORMAT_R32G32B32_SFLOAT  unit (quantised to the 256-entry table by default)
//   color     offset 24  VK_FORMAT_B8G8R8A8_UNORM    D3DCOLOR bytes B,G,R,A: 0xFFFFFFFF, or brightness
//                                                    grey with alpha 0xFF when header 11 is set
//   texcoord  offset 28  VK_FORMAT_R32G32_SFLOAT     world (x, y) (unused by the shaders, which
//                                                    project from position with diffuseTexU/V)
// Same layout as the engine's PrimVertLit FVF (materials programs.hpp).
struct TerrainVertex {
    float position[3];
    float normal[3];
    uint8_t color[4];  // B, G, R, A
    float texcoord[2];
};
static_assert(sizeof(TerrainVertex) == 36);
static_assert(offsetof(TerrainVertex, normal) == 12 && offsetof(TerrainVertex, color) == 24 &&
              offsetof(TerrainVertex, texcoord) == 28);
constexpr uint32_t kTerrainVertexStride = 36;
constexpr uint32_t kBlendTextureSize = 32;                  // texels per side
constexpr uint32_t kBlendTextureBytes = 32 * 32 * 2;        // A4R4G4B4
constexpr uint16_t kBlendTexelOn = 0xFFFF;

struct TerrainPass {
    uint8_t material = 0;                 // slot 0..63 (bind that material's textures/constants)
    std::vector<uint16_t> indices;        // triangle list into the patch vertex buffer
    std::vector<uint8_t> blend_texture;   // kBlendTextureBytes, raw A4R4G4B4 u16 LE
    uint32_t triangle_count() const { return static_cast<uint32_t>(indices.size() / 3); }
    std::span<const uint8_t> index_bytes() const {
        return {reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(uint16_t)};
    }
    uint16_t texel(uint32_t c, uint32_t r) const;
};

struct TerrainPatch {
    uint32_t px = 0, py = 0;              // patch column / row
    std::vector<uint8_t> vertices;        // 1024 * 36 bytes
    assets::Aabb bounds;
    float min_height = 0, max_height = 0;
    float blend_tex_scale[2] = {0, 0};    // blendTexScale
    float blend_tex_offset[2] = {0, 0};   // blendTexOffset
    std::vector<TerrainPass> passes;      // ascending material index
    // All quads that have at least one rendered corner (material -1, no texture). Only filled
    // when the patch has holes ("not rendered" materials); used by decoration passes.
    bool has_holes = false;
    std::vector<uint16_t> rendered_indices;
    const TerrainVertex& vertex(uint32_t c, uint32_t r) const {
        return reinterpret_cast<const TerrainVertex*>(vertices.data())[r * kPatchVerts + c];
    }
};

// Per-material shader constants for the TerrainRenderBump passes (2.5).
struct TerrainMaterialGpu {
    bool used = false;                    // referenced by at least one pass
    std::string diffuse_texture;          // diffuseTexture
    std::string normal_texture;           // normalTexture (Generic_Flat_B.tga when empty)
    std::array<float, 4> tex_u{}, tex_v{};// diffuseTexU / diffuseTexV, dotted with (x, y, z, 1)
    std::array<float, 4> material_diffuse{1, 1, 1, 1};   // (0A, 1)
    std::array<float, 4> material_specular{1, 1, 1, 1};  // (0B, 1)
    float shininess = 64;                 // 06 (unused by `bump`, which hard-codes ^16)
};

struct MeshOptions {
    // Snap normals to the original's 256-entry hemisphere table (2.1). false = smooth float normals.
    bool quantize_normals = true;
};

struct TerrainMeshStats {
    uint32_t patches = 0, passes = 0, max_passes = 0;
    uint64_t triangles = 0;               // sum over all passes (what the GPU rasterises)
    uint64_t vertex_bytes = 0, index_bytes = 0, blend_bytes = 0;
    uint64_t total_bytes() const { return vertex_bytes + index_bytes + blend_bytes; }
};

struct TerrainMesh {
    uint32_t patches_x = 0, patches_y = 0;
    std::vector<TerrainPatch> patches;    // row-major: index = py * patches_x + px
    std::array<TerrainMaterialGpu, kMaterialSlots> materials{};
    TerrainMeshStats stats;
};

// Builds every patch. `parallel` runs patches concurrently (each writes only its own slot), so
// the result is identical for any thread count. False (with *error) if the terrain is absent or
// inconsistent (grid not 31n+1, vertex arrays of the wrong size).
bool build_terrain_mesh(const Terrain& terrain, TerrainMesh& out, const MeshOptions& options = {},
                        const ParallelFor& parallel = serial_for, std::string* error = nullptr);
// One patch (used by build_terrain_mesh; exposed for tests and incremental rebuilds).
void build_patch(const Terrain& terrain, uint32_t px, uint32_t py, const MeshOptions& options, TerrainPatch& out);

// ---- normals (2.1)
// Sobel 1-2-1 gradient of the clamped neighbour heights over a 2-cell baseline, normalised.
std::array<float, 3> terrain_normal(const Terrain& terrain, int64_t x, int64_t y);
// The 256-entry table: entries 0..254 = 2D Halton (base 2 u, base 3 v, index 0 first) mapped to
// (sqrt(1-v) cos 2pi u, sqrt(1-v) sin 2pi u, sqrt v); entry 255 = (0, 0, 1).
const std::array<std::array<float, 3>, 256>& normal_table();
// Index of the table entry with the largest dot product (first on ties).
uint8_t quantize_normal(const std::array<float, 3>& n);

// diffuse UV of a world position for a material (what the shader computes): u = dot(U, (p, 1)).
std::array<float, 2> material_uv(const TerrainMaterial& m, const float (&world)[3]);

// Blend texture as R8 (0 / 255), if a renderer prefers an alpha-only format.
std::vector<uint8_t> blend_texture_r8(const TerrainPass& pass);

}  // namespace alamo::terrain
