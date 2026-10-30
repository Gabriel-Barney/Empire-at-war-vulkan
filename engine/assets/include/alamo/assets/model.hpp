// alamo_assets - ALO model -> GPU-ready CPU data (interleaved vertices, index buffers, bounds,
// material references, per-submesh bone palettes). No Vulkan here: the renderer uploads the byte
// spans as they are.
//
// Every submesh of the file is kept, in file order (mesh order, then submesh order). Hidden,
// collision, shadow-volume and alDefault/Default (proxy) geometry is flagged, never dropped; use
// Submesh::is_render_geometry() for the normal colour pass.
//
// Thread safety: load_model / build_model touch no shared mutable state and may run concurrently
// on worker threads (the FileProvider must allow concurrent const reads, which the VFS and
// DirectoryProvider do).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/assets/skeleton.hpp"
#include "alamo/assets/vertex_layout.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::assets {

// Material parameter as the ALO stores it (chunks 0x10102..0x10106), in declaration order.
enum class ParamType : std::uint8_t { Int = 0, Float = 1, Float3 = 2, Texture = 3, Float4 = 4 };
struct MaterialParam {
    std::string name;
    ParamType type = ParamType::Float;
    std::int32_t int_value = 0;  // Int
    Float4 value;                // Float -> x; Float3 -> xyz (w = 0); Float4 -> xyzw
    std::string texture;         // Texture: file name as written, e.g. "LPf_pelleaonsdf.dds"
};

struct MaterialRef {
    std::string effect;  // e.g. "MeshBumpColorize.fx"
    std::vector<MaterialParam> params;
    const MaterialParam* find(std::string_view name) const;  // case-insensitive
};

enum SubmeshFlags : std::uint32_t {
    kSubmeshHidden = 1u << 0,           // mesh is_hidden (0x402): runtime-toggled (muzzle flash ...)
    kSubmeshCollisionMesh = 1u << 1,    // mesh is_collision_enabled (0x402)
    kSubmeshCollisionShader = 1u << 2,  // effect MeshCollision.fx (debug geometry)
    kSubmeshShadowVolume = 1u << 3,     // effect *ShadowVolume.fx (stencil shadow pass only)
    kSubmeshProxyShader = 1u << 4,      // effect alDefault.fx / Default.fx (not drawn, CATALOG 4.2)
    kSubmeshSkinned = 1u << 5,          // VertexLayout::Skinned
    kSubmeshHasCollisionTree = 1u << 6, // 0x1200 present
    kSubmeshUnknownFormat = 1u << 7,    // vertex format name not in the table (treated as static)
};

// What the ALO vertex-format name (0x10002) says, for mapping to a shader/material vertex layout.
// All names seen in the shipped data (FoC + EaW GameData via the VFS, 9294 submeshes; the
// real-data test prints these counts). Layout = the VertexLayout they load into.
//   name                    count  layout   meaning
//   alD3dVertN               2054  Static   normal only (shadow volumes, collision, untextured)
//   alD3dVertNU2             4455  Static   normal + uv0 (most common)
//   alD3dVertNU2C             104  Static   + vertex colour
//   alD3dVertNU2U3U3         1977  Static   + authored tangent/binormal (bump)
//   alD3dVertNU2U3U3C          17  Static   + tangent frame + colour
//   alD3dVertGrass             14  Static   normal + uv0 (grass bend)
//   alD3dVertRSkinNU2         423  Skinned  rigid skin
//   alD3dVertRSkinNU2U3U3     198  Skinned  rigid skin + tangent frame
//   alD3dVertB4I4NU2           47  Skinned  legacy EaW 4-bone name, weights 1,0,0,0 (loaded rigid)
//   alD3dVertB4I4NU2U3U3        5  Skinned  same + tangent frame
// Renderer mapping: skinned -> SKINNED permutation; tangent_frame -> BUMP inputs; color -> vertex
// colour. With ModelLoadOptions::vertex_type_override these traits describe the EFFECTIVE vertex
// type (the effect's _ALAMO_VERTEX_TYPE), as in the original loader (SPACE_AND_OPEN.md D.2). Decoded by alamo_formats
// (alo::vertex_format_info): unknown names get known = false and fields inferred from the tokens
// (U2, U3U3, Skin/B4I4, trailing C).
struct VertexFormatTraits {
    bool known = false;
    bool normal = true;
    int uv_sets = 0;
    bool tangent_frame = false;  // tangent + binormal authored
    bool color = false;
    bool skinned = false;        // RSkin / B4I4
    bool grass = false;
};

struct Submesh {
    std::uint32_t mesh = 0;            // index into ModelAsset::meshes
    std::uint32_t index_in_mesh = 0;
    std::uint32_t flags = 0;           // SubmeshFlags
    std::string vertex_format;         // e.g. "alD3dVertRSkinNU2U3U3", exactly as stored in the ALO
    // The vertex type the vertices were packed as: vertex_format, or the override returned by
    // ModelLoadOptions::vertex_type_override (the effect's _ALAMO_VERTEX_TYPE, D.2).
    std::string effective_vertex_format;
    bool vertex_type_overridden = false;  // effective_vertex_format != vertex_format (case-insensitive)
    VertexFormatTraits format;         // decoded from effective_vertex_format
    // The effective format carries a tangent frame: tangent/binormal are the record's values AS
    // STORED (may be zero, e.g. the 42 Death Star II submeshes; never repaired). False: the
    // vertices hold zero T/B (unless ModelLoadOptions::generate_tangent_frames).
    bool has_tangents = false;
    VertexLayout layout = VertexLayout::Static;
    std::uint32_t vertex_count = 0;
    std::vector<std::uint8_t> vertices;  // vertex_count * vertex_stride(layout) bytes
    IndexType index_type = IndexType::U16;
    std::uint32_t index_count = 0;       // triangle list
    std::vector<std::uint8_t> indices;   // index_count * index_size(index_type) bytes
    Aabb bounds;                         // of the positions, in the vertex space (see layouts)
    Sphere sphere;                       // centred on bounds, encloses every vertex
    MaterialRef material;
    // Skinned: submesh-local palette index -> skeleton bone (the ALO 0x10006 map, <= 24 entries in
    // shipped data). Empty for static submeshes.
    std::vector<std::uint16_t> bone_palette;
    // Static: the bone whose world matrix places the vertices. Skinned: 0 (vertices are in model
    // space; use the palette).
    std::uint32_t attach_bone = 0;

    bool skinned() const { return layout == VertexLayout::Skinned; }
    bool is_render_geometry() const {
        return (flags & (kSubmeshCollisionShader | kSubmeshShadowVolume | kSubmeshProxyShader)) == 0;
    }
    std::span<const std::uint8_t> vertex_bytes() const { return vertices; }
    std::span<const std::uint8_t> index_bytes() const { return indices; }
};

struct MeshInfo {
    std::string name;
    Aabb file_bounds;           // 0x402 box as stored (mesh space)
    bool hidden = false;
    bool collision_enabled = false;
    int lod = -1;               // n for names ending "_LOD<n>", else -1
    std::uint32_t attach_bone = 0;
    std::uint32_t first_submesh = 0;
    std::uint32_t submesh_count = 0;
};

struct Proxy {
    std::string name;  // .alo/.alp to attach, without extension
    std::uint32_t bone = 0;
    bool hidden = false;
};

struct ModelAsset {
    std::string name;  // path it was loaded from (normalised) or the name given to build_model
    Skeleton skeleton;
    std::vector<MeshInfo> meshes;
    std::vector<Submesh> submeshes;
    std::vector<Proxy> proxies;
    // Bind-pose model-space bounds of the render geometry (hidden included; shadow/collision/proxy
    // excluded). Invalid (min > max) if there is none.
    Aabb bounds;
    Sphere sphere;
    // Number of submeshes stored as legacy 128-byte vertices (EaW-era chunk 0x10005).
    std::uint32_t legacy_vertex_chunks = 0;
    std::vector<std::string> warnings;

    // Distinct texture names referenced by any material, sorted case-insensitively, deduplicated.
    std::vector<std::string> texture_names() const;
};

// Per-submesh vertex-type override: given the submesh's material, return the vertex type the
// effect declares (`_ALAMO_VERTEX_TYPE`, e.g. "alD3dVertNU2U3U3"), or an empty string to keep the
// ALO's format. The original loader replaces the ALO format string with the effect's type when the
// effect is a standard alD3dEffect with a known type, and packs the stored records into that layout
// (SPACE_AND_OPEN.md D.2; Planet.fx makes W_PLANET_DEATHSTAR_II_DESTROYED's alD3dVertN submeshes
// NU2U3U3 with stored UVs and T = B = 0). Return empty for a missing / failed effect (D.3) and for
// a name alamo_formats does not know (such a name is ignored with a warning). Called from the
// loading thread: must be thread-safe and deterministic (same material -> same answer).
using VertexTypeOverride = std::function<std::string(const MaterialRef& material)>;

struct ModelLoadOptions {
    VertexTypeOverride vertex_type_override;  // empty = use the ALO's formats
    // Debug / enhancement only, OFF for parity: repair zero authored frames and generate frames for
    // formats without one (compute_tangent_frame). The original engine generates nothing (D.5).
    bool generate_tangent_frames = false;
};

// Parse an in-memory .alo. Returns false (with *error) on a malformed file or a particle system.
//
// Vertex rules (original loader, SPACE_AND_OPEN.md D.4/D.5):
//   * tangent / binormal: effective format with a frame -> the stored vectors verbatim (zero stays
//     zero; no repair, no generation, no handedness rule); without a frame -> zero. Shaders must
//     tolerate zero T/B (the engine's SafeNormalize turns the projected vector into (0, 0, +-1)).
//   * colour: legacy 0x10005 (128-byte) records -> (1, 1, 1, 1) always, the stored colour is
//     discarded by the original converter; 0x10007 (144-byte) records -> stored colour if the
//     effective format has C, else white.
//   * uv0 is copied from the record for every format (shaders of formats without UVs ignore it).
bool build_model(std::span<const std::uint8_t> bytes, std::string_view name, ModelAsset& out,
                 std::string* error = nullptr);
bool build_model(std::span<const std::uint8_t> bytes, std::string_view name, ModelAsset& out,
                 const ModelLoadOptions& options, std::string* error = nullptr);
// Read through the FileProvider (VFS) and build. `path` as for model_path().
bool load_model(const FileProvider& files, std::string_view path, ModelAsset& out,
                std::string* error = nullptr);
bool load_model(const FileProvider& files, std::string_view path, ModelAsset& out,
                const ModelLoadOptions& options, std::string* error = nullptr);

// Generate a tangent frame (used only with ModelLoadOptions::generate_tangent_frames). With UVs (uv.size() >= vertex count): per-triangle dP/du and dP/dv
// accumulated per vertex, then orthonormalised against the normal (binormal = +-normal x tangent,
// sign from the accumulated dP/dv). Vertices without a usable UV gradient (or uv empty) get an
// arbitrary orthonormal basis around the normal (zero normals treated as +Z). Output is unit length.
void compute_tangent_frame(std::span<const Float3> pos, std::span<const Float3> nrm, std::span<const Float2> uv,
                           std::span<const std::uint16_t> indices, std::span<Float3> tangent, std::span<Float3> binormal);
// Replace zero-length / non-finite authored tangent-frame vectors with a valid basis.
void repair_tangent_frame(std::span<const Float3> nrm, std::span<Float3> tangent, std::span<Float3> binormal);

// True if the bytes are an ALO particle system (first chunk 0x900) rather than a model.
bool is_particle_alo(std::span<const std::uint8_t> bytes);

// "EI_ROYALGAURD" / "ei_royalgaurd.alo" -> "DATA\ART\MODELS\EI_ROYALGAURD.ALO". A name that already
// contains a path separator is only normalised (and gets ".ALO" if it has no extension).
std::string model_path(std::string_view name);
// Same for animations (Data\Art\Models\<name>.ala).
std::string animation_path(std::string_view name);

}  // namespace alamo::assets
