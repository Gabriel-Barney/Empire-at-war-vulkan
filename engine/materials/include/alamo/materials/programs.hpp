// Registry of the SPIR-V programs compiled at build time from engine/materials/shaders
// (PORTING.md track A hot set + the fixed-function ubershader of 3.4).
//
// Effects outside the hot set return ProgramStatus::NeedsTranslation: they go through track B
// (automatic SM1-3 -> SPIR-V translation of the FXO bytecode, a later task) using the generic
// register-file interface (constants.hpp VSConstants/PSConstants, register_file.hpp).
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "alamo/materials/pso.hpp"

namespace alamo::materials {

enum class ShaderStage : uint8_t { Vertex, Pixel };

struct ShaderBlob {
    const char* name;           // e.g. "mesh_vs_bump"
    const char* source;         // HLSL file under engine/materials/shaders
    const char* entry_point;    // SPIR-V OpEntryPoint name ("VSMain" / "PSMain")
    const char* defines;        // comma-separated permutation defines
    ShaderStage stage;
    std::span<const uint32_t> spirv;
    uint32_t id;                // stable program id (FNV-1a of the name), used in PsoKey
    uint64_t content_hash;      // FNV-1a of the SPIR-V words (pipeline-cache invalidation)
};

// Vertex input locations used by every hand-written / FF shader (the shader-side type is always
// float; VertexInput::format names the VkFormat the vertex attribute must use):
//   0 POSITION  float3  R32G32B32_SFLOAT
//   1 NORMAL    float4  R32G32B32A32_SFLOAT  .w = rigid bone index (float, truncated) for RSkin
//   2 TEXCOORD0 float2  R32G32_SFLOAT
//   3 TANGENT   float3  R32G32B32_SFLOAT     (zero basis is tolerated: SafeNormalize)
//   4 BINORMAL  float3  R32G32B32_SFLOAT
//   5 COLOR0    float4  rgba in the shader. Mesh vertices (alamo_assets Static/Skinned layouts,
//               converted from ALO floats to R,G,B,A bytes): VK_FORMAT_R8G8B8A8_UNORM, offset 56
//               (static) / 60 (skinned). D3DCOLOR sources (alPrimVert particles, CPU-generated
//               B,G,R,A bytes): VK_FORMAT_B8G8R8A8_UNORM. Either way the shader sees (r,g,b,a).
// Static normal is R32G32B32 (w reads 1.0, unused); skinned normal is R32G32B32A32 with
// w = submesh-local palette index. vertex_layout_desc() gives the full attribute table.
enum VertexLocation : uint32_t { kLocPosition = 0, kLocNormal = 1, kLocTexcoord0 = 2, kLocTangent = 3,
                                 kLocBinormal = 4, kLocColor0 = 5 };

struct VertexInput {
    uint32_t location;
    const char* semantic;
    uint32_t components;
    const char* format;   // VkFormat name expected for the attribute
    const char* note;
};

// Vertex attribute table per layout (location, VkFormat value + name, byte offset). Mesh layouts
// mirror engine/assets vertex_layout.hpp revision 2 (both 64-byte strides): every non-RSkin
// layout uses the assets Static layout, every RSkin layout the Skinned layout. PrimVert /
// PrimVertLit are the engine's dynamic FVF buffers (RENDER.md 1.4): XYZ|DIFFUSE|TEX1 (24 B) and
// XYZ|NORMAL|DIFFUSE|TEX1 (36 B), colour D3DCOLOR = B8G8R8A8_UNORM.
struct VertexAttributeDesc {
    uint32_t location;
    uint32_t vk_format;        // VkFormat numeric value
    const char* format_name;
    uint32_t offset;
};
struct VertexLayoutDesc {
    uint32_t stride = 0;
    std::span<const VertexAttributeDesc> attributes;
};
VertexLayoutDesc vertex_layout_desc(VertexLayout l);
namespace vkfmt {
constexpr uint32_t kR8G8B8A8Unorm = 37, kB8G8R8A8Unorm = 44, kR32G32Sfloat = 103, kR32G32B32Sfloat = 106,
                   kR32G32B32A32Sfloat = 109;
}

enum class BindingKind : uint8_t { UniformBuffer, StorageBuffer, SampledImageArray, SamplerArray, CombinedImageSampler };
struct ResourceBinding {
    uint32_t set, binding;
    BindingKind kind;
    const char* name;           // FrameCB, Draws, Materials, Bones, FfDraws, Textures, Samplers
    uint32_t count;             // array size, 0 = runtime-sized (descriptor indexing)
    const char* note;
};

struct ProgramInterface {
    const char* name;
    std::span<const VertexInput> inputs;
    std::span<const ResourceBinding> bindings;
    uint32_t push_constant_bytes;   // PushConstants, VS + PS stages
    const char* notes;
};

enum class ProgramFamily : uint8_t { Mesh, Planet, ShadowVolume, FixedFunction };

enum Permutation : uint32_t {
    kPermDefault = 0,
    kPermSkinned = 1u << 0,     // RSkin variant of a mesh / shadow-volume effect
    kPermFfNormal = 1u << 8,    // FF ubershader: vertex has NORMAL (FF programs only)
    kPermFfColor = 1u << 9,     // FF ubershader: vertex has COLOR0
    kPermFfSkinned = 1u << 10,  // FF ubershader: rigid skinning from NORMAL.w (implies kPermFfNormal)
    kPermFfInputs = kPermFfNormal | kPermFfColor | kPermFfSkinned,
};

struct Program {
    const ShaderBlob* vs = nullptr;
    const ShaderBlob* ps = nullptr;
    const ProgramInterface* iface = nullptr;
    ProgramFamily family = ProgramFamily::Mesh;
    VertexLayout vertex_layout = VertexLayout::Unknown;  // expected layout of the effect
    uint32_t permutation = 0;
};

enum class ProgramStatus : uint8_t {
    Ok,
    NeedsTranslation,   // not in the hot set: track B (automatic translation), later task
    UnknownPermutation, // in the hot set, but that permutation is not compiled
    Translated,         // track B: pass bytecode translated on demand (translated.hpp translate_pass)
};
struct ProgramLookup {
    ProgramStatus status = ProgramStatus::NeedsTranslation;
    Program program;
};

std::span<const ShaderBlob> all_shader_blobs();
const ShaderBlob* find_shader_blob(std::string_view name);
// Mix of all content hashes: changes whenever any shader changes (pipeline-cache header).
uint64_t programs_fingerprint();

// effect_name: "MeshBumpColorize", "MeshBumpColorize.fx", "Data\\Art\\Shaders\\MESHALPHA.FXO" ...
ProgramLookup find_program(std::string_view effect_name, uint32_t permutation = kPermDefault);
// The FF ubershader pair for a set of vertex inputs (kPermFfNormal / kPermFfColor bits).
Program ff_program(uint32_t ff_inputs);
uint32_t ff_inputs_for_layout(VertexLayout l);

struct HotEffect {
    const char* effect;         // FXO name (upper case, no extension)
    const char* vs;             // ShaderBlob names
    const char* ps;
    ProgramFamily family;
    VertexLayout layout;
    uint32_t combined_drawn;    // CATALOG 4.1 combined corpus drawn submeshes (0 = engine-driven)
};
std::span<const HotEffect> hot_effects();
constexpr uint32_t kCatalogCombinedDrawn = 6911;

}  // namespace alamo::materials
