// alamo_assets - the two fixed GPU vertex layouts produced from ALO submeshes.
//
// Every ALO submesh stores the same 144-byte master vertex (SPEC.md 2.5); the vertex format name
// (0x10002) only says which fields are meaningful. We repack into one of two 64-byte layouts:
//
// VertexLayout::Static (all non-RSkin formats: alD3dVertN, NU2, NU2C, NU2U3U3, Grass)
//   off  size  field      VkFormat                     notes
//    0    12   position   VK_FORMAT_R32G32B32_SFLOAT   space of the mesh's attach bone (Submesh::attach_bone)
//   12    12   normal     VK_FORMAT_R32G32B32_SFLOAT   as stored (zero on shadow-volume fin vertices)
//   24     8   uv0        VK_FORMAT_R32G32_SFLOAT      texcoord set 0 as stored (no V flip)
//   32    12   tangent    VK_FORMAT_R32G32B32_SFLOAT   as stored for *U3U3 formats (may be zero), else zero
//   44    12   binormal   VK_FORMAT_R32G32B32_SFLOAT   as stored for *U3U3 formats (may be zero), else zero
//   56     4   color      VK_FORMAT_R8G8B8A8_UNORM     RGBA; stored colour if the format has C and the
//                                                      record is 144-byte 0x10007; else 255,255,255,255
//                                                      (legacy 0x10005 records are always white, D.4)
//   60     4   (padding, zero)
//
// VertexLayout::Skinned (alD3dVertRSkin*; rigid skinning, one bone per vertex, weight 1)
//   off  size  field      VkFormat                     notes
//    0    12   position   VK_FORMAT_R32G32B32_SFLOAT   model (bind) space, NOT bone-local (SPEC.md 2.6)
//   12    16   normal     VK_FORMAT_R32G32B32A32_SFLOAT xyz = normal as stored,
//                                                      w = submesh-local palette index (0..23) as a
//                                                      float, like the original's NORMAL.w packing;
//                                                      shader: idx = uint(normal.w), matrix =
//                                                      palette[idx] (Submesh::bone_palette order)
//   28     8   uv0        VK_FORMAT_R32G32_SFLOAT
//   36    12   tangent    VK_FORMAT_R32G32B32_SFLOAT   same rules as Static
//   48    12   binormal   VK_FORMAT_R32G32B32_SFLOAT
//   60     4   color      VK_FORMAT_R8G8B8A8_UNORM     same rules as Static
//   The skeleton bone of a vertex is Submesh::bone_palette[uint(normal.w)]; a renderer that wants a
//   single per-instance palette SSBO (PORTING.md 2.4) remaps on the CPU or through a per-draw
//   palette-offset table. (Revision 2: colour replaced the former u16 skeleton-bone field at 60,
//   which no shader read.)
//
// Tangent frame (both layouts), original-engine rule (SPACE_AND_OPEN.md B.7/D.5): the engine never
// generates or repairs one. Submesh::has_tangents = the effective format (after the effect's
// _ALAMO_VERTEX_TYPE override, D.2) has a frame; then T and B are the stored vectors verbatim, used
// as two independent vectors (not unit, not orthogonal, no handedness sign; may be zero). Otherwise
// T = B = 0. Shaders must accept zero T/B without producing NaN (SafeNormalize of the projected
// vector). ModelLoadOptions::generate_tangent_frames restores the old repair/generation (debug only).
//
// Both layouts are tightly packed little-endian structs: Submesh::vertices is a byte copy of an
// array of these and can be memcpy'd straight into a staging buffer. Binding stride = 64.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace alamo::assets {

enum class VertexLayout : std::uint8_t { Static = 0, Skinned = 1 };

struct StaticVertex {
    float position[3];
    float normal[3];
    float uv0[2];
    float tangent[3];
    float binormal[3];
    std::uint8_t color[4];
    std::uint32_t pad;
};

struct SkinnedVertex {
    float position[3];
    float normal[4];  // w = submesh-local palette index
    float uv0[2];
    float tangent[3];
    float binormal[3];
    std::uint8_t color[4];
};

static_assert(sizeof(StaticVertex) == 64);
static_assert(offsetof(StaticVertex, normal) == 12 && offsetof(StaticVertex, uv0) == 24 &&
              offsetof(StaticVertex, tangent) == 32 && offsetof(StaticVertex, binormal) == 44 &&
              offsetof(StaticVertex, color) == 56);
static_assert(sizeof(SkinnedVertex) == 64);
static_assert(offsetof(SkinnedVertex, normal) == 12 && offsetof(SkinnedVertex, uv0) == 28 &&
              offsetof(SkinnedVertex, tangent) == 36 && offsetof(SkinnedVertex, binormal) == 48 &&
              offsetof(SkinnedVertex, color) == 60);

constexpr std::uint32_t vertex_stride(VertexLayout) { return 64; }

// Attribute table so the renderer can build VkVertexInputAttributeDescription without hard-coding
// offsets. Shader input locations: the index in the table.
enum class VertexSemantic : std::uint8_t { Position, Normal, TexCoord0, Tangent, Binormal, Color };
// AttribFormat -> VkFormat:
//   Float2 -> VK_FORMAT_R32G32_SFLOAT        Float3 -> VK_FORMAT_R32G32B32_SFLOAT
//   Float4 -> VK_FORMAT_R32G32B32A32_SFLOAT  UNorm8x4 -> VK_FORMAT_R8G8B8A8_UNORM
enum class AttribFormat : std::uint8_t { Float2, Float3, Float4, UNorm8x4 };
struct VertexAttribute {
    VertexSemantic semantic;
    AttribFormat format;
    std::uint32_t offset;
};
std::span<const VertexAttribute> vertex_attributes(VertexLayout layout);

// Index buffer element type. U16 -> VK_INDEX_TYPE_UINT16, U32 -> VK_INDEX_TYPE_UINT32.
enum class IndexType : std::uint8_t { U16 = 0, U32 = 1 };
constexpr std::uint32_t index_size(IndexType t) { return t == IndexType::U16 ? 2u : 4u; }

}  // namespace alamo::assets
