// SPIR-V program registry. See programs.hpp. The blobs come from generated headers
// (CMakeLists.txt ALAMO_MATERIAL_PROGRAMS -> generated/alamo_materials_spirv/*.h).
#include "alamo/materials/programs.hpp"

#include <array>
#include <string>
#include <vector>

#include "alamo/core/hash.hpp"
#include "alamo/materials/constants.hpp"

#define ALAMO_SPV(name, src, ep, stage, defs)
#include "programs.inc"
#undef ALAMO_SPV

namespace alamo::materials {
namespace {

constexpr uint64_t fnv64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}
constexpr uint32_t program_id(std::string_view name) {
    const uint64_t h = fnv64(name);
    // Hand-written programs: bit 31 clear (kProgramIdTranslatedBit marks track-B bytecode ids).
    const uint32_t id = static_cast<uint32_t>(h ^ (h >> 32)) & ~kProgramIdTranslatedBit;
    return id == 0 ? 1u : id;  // 0 = unresolved in PsoKey
}

const std::vector<ShaderBlob>& blobs() {
    static const std::vector<ShaderBlob> table = [] {
        std::vector<ShaderBlob> t;
#define ALAMO_SPV(name, src, ep, stg, defs)                                                         \
    t.push_back(ShaderBlob{#name, src, ep, defs, ShaderStage::stg,                                   \
                           std::span<const uint32_t>(spv_##name, sizeof(spv_##name) / sizeof(uint32_t)), \
                           program_id(#name), alamo::fnv1a64(spv_##name, sizeof(spv_##name))});
#include "programs.inc"
#undef ALAMO_SPV
        return t;
    }();
    return table;
}

constexpr const char* kF3 = "VK_FORMAT_R32G32B32_SFLOAT";
constexpr const char* kF4 = "VK_FORMAT_R32G32B32A32_SFLOAT";
constexpr const char* kF2 = "VK_FORMAT_R32G32_SFLOAT";
constexpr const char* kColorFormat =
    "VK_FORMAT_R8G8B8A8_UNORM for mesh vertices (alamo_assets RGBA, offset 56 static / 60 skinned); "
    "VK_FORMAT_B8G8R8A8_UNORM for D3DCOLOR sources (alPrimVert); shader reads float4 rgba";

constexpr VertexInput kMeshInputs[] = {
    {kLocPosition, "POSITION", 3, kF3, "object space"},
    {kLocNormal, "NORMAL", 4, kF4, "skinned: R32G32B32A32, .w = palette index; static: R32G32B32 (w reads 1)"},
    {kLocTexcoord0, "TEXCOORD0", 2, kF2, ""},
    {kLocTangent, "TANGENT", 3, kF3, "BUMP variants / Planet only; zero basis tolerated"},
    {kLocBinormal, "BINORMAL", 3, kF3, "BUMP variants / Planet only; zero basis tolerated"},
};
constexpr VertexInput kShadowInputs[] = {
    {kLocPosition, "POSITION", 3, kF3, "object space"},
    {kLocNormal, "NORMAL", 4, kF4, "face normal of the fin/cap vertex; .w = bone index (RSkin)"},
};
constexpr VertexInput kFfInputs[] = {
    {kLocPosition, "POSITION", 3, kF3, "object space (identity World for particles)"},
    {kLocNormal, "NORMAL", 4, kF4, "kPermFfNormal only; .w = bone index with kPermFfSkinned"},
    {kLocTexcoord0, "TEXCOORD0", 2, kF2, ""},
    {kLocColor0, "COLOR0", 4, kColorFormat, "kPermFfColor only (also RSkinNU2C, whatever its byte offset)"},
};
using namespace vkfmt;
constexpr VertexAttributeDesc kStaticAttrs[] = {
    {kLocPosition, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {kLocNormal, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 12},
    {kLocTexcoord0, kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 24},
    {kLocTangent, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 32},
    {kLocBinormal, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 44},
    {kLocColor0, kR8G8B8A8Unorm, "VK_FORMAT_R8G8B8A8_UNORM", 56},
};
constexpr VertexAttributeDesc kSkinnedAttrs[] = {
    {kLocPosition, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {kLocNormal, kR32G32B32A32Sfloat, "VK_FORMAT_R32G32B32A32_SFLOAT", 12},
    {kLocTexcoord0, kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 28},
    {kLocTangent, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 36},
    {kLocBinormal, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 48},
    {kLocColor0, kR8G8B8A8Unorm, "VK_FORMAT_R8G8B8A8_UNORM", 60},
};
// RENDER.md 1.4: dynamic geometry buffers created at init, CPU-expanded particles / lasers:
//   FVF XYZ|DIFFUSE|TEX1         (24 B): pos @0, D3DCOLOR @12, uv @16   ("pre-lit")
//   FVF XYZ|NORMAL|DIFFUSE|TEX1  (36 B): pos @0, normal @12, D3DCOLOR @24, uv @28   ("lit")
constexpr VertexAttributeDesc kPrimAttrs[] = {
    {kLocPosition, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {kLocColor0, kB8G8R8A8Unorm, "VK_FORMAT_B8G8R8A8_UNORM", 12},
    {kLocTexcoord0, kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 16},
};
constexpr VertexAttributeDesc kPrimLitAttrs[] = {
    {kLocPosition, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {kLocNormal, kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 12},
    {kLocColor0, kB8G8R8A8Unorm, "VK_FORMAT_B8G8R8A8_UNORM", 24},
    {kLocTexcoord0, kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 28},
};

constexpr ResourceBinding kMeshBindings[] = {
    {0, 0, BindingKind::UniformBuffer, "FrameCB", 1, "constants.hpp FrameCB (992 B, incl. SH matrices)"},
    {0, 1, BindingKind::StorageBuffer, "Draws", 1, "DrawData[] (80 B), index PC.draw_index + SV_InstanceID"},
    {0, 2, BindingKind::StorageBuffer, "Materials", 1, "MeshMaterial[] (96 B), deduplicated MaterialTable"},
    {0, 3, BindingKind::StorageBuffer, "Bones", 1,
     "BoneMatrix[] (48 B); skinned: Bones[DrawData.bone_base + trunc(NORMAL.w)]"},
    {1, 0, BindingKind::SampledImageArray, "Textures", 0, "bindless Texture2D[], NonUniformResourceIndex"},
    {1, 1, BindingKind::SamplerArray, "Samplers", kEngineSamplerCount, "EngineSampler slots"},
};
constexpr ResourceBinding kFfBindings[] = {
    {0, 0, BindingKind::UniformBuffer, "FrameCB", 1, "constants.hpp FrameCB"},
    {0, 1, BindingKind::StorageBuffer, "Draws", 1, "DrawData[]; material_index selects the FfDrawData record"},
    {0, 3, BindingKind::StorageBuffer, "Bones", 1, "BoneMatrix[]; kPermFfSkinned only: Bones[bone_base + trunc(NORMAL.w)]"},
    {0, 4, BindingKind::StorageBuffer, "FfDraws", 1, "FfDrawData[] (432 B), built per draw by build_ff_draw_data"},
    {1, 0, BindingKind::SampledImageArray, "Textures", 0, "bindless Texture2D[]; slot 0 (kNoTexture) = 1x1 white"},
    {1, 1, BindingKind::SamplerArray, "Samplers", kEngineSamplerCount, "EngineSampler slots"},
};

constexpr const char* kSkinNote =
    "Rigid skinning, 1 bone per vertex, no weights: bone index = trunc(NORMAL.w) as in the vs_1_1 "
    "originals; palette entry = Bones[DrawData.bone_base + index]. NORMAL.w is submesh-local "
    "(ALO 0x10006 bone map), so bone_base must point at a slice in bone-map order. The 24-bone "
    "limit of the original (c0..c71) does not apply.";

const ProgramInterface kMeshIface{"AlamoMesh", kMeshInputs, kMeshBindings, sizeof(PushConstants), kSkinNote};
const ProgramInterface kPlanetIface{"Planet", kMeshInputs, kMeshBindings, sizeof(PushConstants),
                                    "Requires alD3dVertNU2U3U3 (tangent frame)."};
const ProgramInterface kShadowIface{"ShadowVolume", kShadowInputs, kMeshBindings, sizeof(PushConstants), kSkinNote};
const ProgramInterface kFfIface{"FixedFunction", kFfInputs, kFfBindings, sizeof(PushConstants),
                                "D3D9 FF emulation driven by FfDrawData; one SPIR-V pair per vertex-input set. kPermFfSkinned: "
                                "same rigid NORMAL.w palette convention as AlamoMesh."};

constexpr HotEffect kHot[] = {
    {"MESHBUMPCOLORIZE", "mesh_vs_bump", "mesh_ps_bump_colorize", ProgramFamily::Mesh, VertexLayout::NU2U3U3, 1682},
    {"MESHALPHA", "ff_vs_n", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::NU2, 1264},
    {"MESHSHADOWVOLUME", "shadow_vs", "shadow_ps", ProgramFamily::ShadowVolume, VertexLayout::N, 1219},
    {"MESHADDITIVE", "mesh_vs_additive", "mesh_ps_additive", ProgramFamily::Mesh, VertexLayout::NU2, 844},
    {"PLANET", "planet_vs", "planet_ps", ProgramFamily::Planet, VertexLayout::NU2U3U3, 296},
    {"MESHGLOSS", "mesh_vs_gloss", "mesh_ps_gloss", ProgramFamily::Mesh, VertexLayout::NU2, 260},
    {"RSKINBUMPCOLORIZE", "mesh_vs_bump_skin", "mesh_ps_bump_colorize", ProgramFamily::Mesh,
     VertexLayout::RSkinNU2U3U3, 203},
    {"MESHGLOSSCOLORIZE", "mesh_vs_gloss", "mesh_ps_gloss_colorize", ProgramFamily::Mesh, VertexLayout::NU2, 185},
    {"TREE", "mesh_vs_tree", "mesh_ps_tree", ProgramFamily::Mesh, VertexLayout::NU2, 185},
    {"RSKINSHADOWVOLUME", "shadow_vs_skin", "shadow_ps", ProgramFamily::ShadowVolume, VertexLayout::RSkinNU2, 182},
    {"RSKINGLOSS", "mesh_vs_gloss_skin", "mesh_ps_gloss", ProgramFamily::Mesh, VertexLayout::RSkinNU2, 146},
    {"RSKINGLOSSCOLORIZE", "mesh_vs_gloss_skin", "mesh_ps_gloss_colorize", ProgramFamily::Mesh,
     VertexLayout::RSkinNU2, 69},
    {"RSKINADDITIVE", "mesh_vs_additive_skin", "mesh_ps_additive", ProgramFamily::Mesh, VertexLayout::RSkinNU2, 42},
    // Fixed-function-only effects (best technique FIXEDFUNCTION): FF ubershader.
    {"MESHALPHASCROLL", "ff_vs_n", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::NU2, 2},
    {"PRIMALPHA", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"PRIMADDITIVE", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    // Lit particles: LIGHTING + COLORVERTEX need normals -> the 36-byte XYZ|NORMAL|DIFFUSE|TEX1 buffer.
    {"PRIMDIFFUSEALPHA", "ff_vs_nc", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVertLit, 0},
    {"PRIMOPAQUE", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"PRIMALPHASCANLINES", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"BLOBSTENCILMASKED", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"FRAMEEFFECTALPHA", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"FRAMEEFFECTWIPE", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"SPACEFOGOFWAR", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"STENCILDARKEN", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"STENCILDARKENTOALPHA", "ff_vs_c", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::PrimVert, 0},
    {"TERRAINPASSABILITY", "ff_vs_n", "ff_ps", ProgramFamily::FixedFunction, VertexLayout::Terrain, 0},
};

std::string effect_key(std::string_view n) {
    size_t slash = n.find_last_of("\\/");
    if (slash != std::string_view::npos) n = n.substr(slash + 1);
    size_t dot = n.find_last_of('.');
    if (dot != std::string_view::npos) n = n.substr(0, dot);
    std::string k(n);
    for (char& c : k)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return k;
}

const ProgramInterface* iface_for(ProgramFamily f) {
    switch (f) {
    case ProgramFamily::Mesh: return &kMeshIface;
    case ProgramFamily::Planet: return &kPlanetIface;
    case ProgramFamily::ShadowVolume: return &kShadowIface;
    default: return &kFfIface;
    }
}

}  // namespace

std::span<const ShaderBlob> all_shader_blobs() { return blobs(); }

const ShaderBlob* find_shader_blob(std::string_view name) {
    for (const auto& b : blobs())
        if (name == b.name) return &b;
    return nullptr;
}

uint64_t programs_fingerprint() {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& b : blobs()) h = alamo::fnv1a64(&b.content_hash, sizeof b.content_hash, h);
    return h;
}

std::span<const HotEffect> hot_effects() { return kHot; }

VertexLayoutDesc vertex_layout_desc(VertexLayout l) {
    if (vertex_layout_skinned(l)) return {64, kSkinnedAttrs};
    if (l == VertexLayout::PrimVert) return {24, kPrimAttrs};
    if (l == VertexLayout::PrimVertLit) return {36, kPrimLitAttrs};
    if (l == VertexLayout::Unknown || l == VertexLayout::Terrain || l == VertexLayout::Count) return {};
    return {64, kStaticAttrs};
}

uint32_t ff_inputs_for_layout(VertexLayout l) {
    switch (l) {
    case VertexLayout::N:
    case VertexLayout::NU2:
    case VertexLayout::NU2U3U3:
    case VertexLayout::Terrain:
        return kPermFfNormal;
    case VertexLayout::RSkinNU2:
    case VertexLayout::RSkinNU2U3U3:
        return kPermFfNormal | kPermFfSkinned;
    case VertexLayout::NU2C:
    case VertexLayout::NU2U3U3C:
        return kPermFfNormal | kPermFfColor;
    case VertexLayout::RSkinNU2C:
        return kPermFfNormal | kPermFfColor | kPermFfSkinned;
    case VertexLayout::PrimVert:
        return kPermFfColor;
    case VertexLayout::PrimVertLit:
        return kPermFfNormal | kPermFfColor;
    default:
        return 0;
    }
}

Program ff_program(uint32_t inputs) {
    Program p;
    const bool skin = (inputs & kPermFfSkinned) != 0;
    const bool n = skin || (inputs & kPermFfNormal) != 0, c = (inputs & kPermFfColor) != 0;
    if (skin) inputs |= kPermFfNormal;
    p.vs = find_shader_blob(skin ? (c ? "ff_vs_nc_skin" : "ff_vs_n_skin")
                                 : n ? (c ? "ff_vs_nc" : "ff_vs_n") : (c ? "ff_vs_c" : "ff_vs"));
    p.ps = find_shader_blob("ff_ps");
    p.iface = &kFfIface;
    p.family = ProgramFamily::FixedFunction;
    p.permutation = inputs & kPermFfInputs;
    return p;
}

ProgramLookup find_program(std::string_view effect_name, uint32_t perm) {
    ProgramLookup r;
    const std::string key = effect_key(effect_name);
    const HotEffect* h = nullptr;
    for (const auto& e : kHot)
        if (key == e.effect) h = &e;
    if (!h) {
        r.status = ProgramStatus::NeedsTranslation;
        return r;
    }
    if (h->family == ProgramFamily::FixedFunction) {
        if (perm & ~static_cast<uint32_t>(kPermFfInputs)) {
            r.status = ProgramStatus::UnknownPermutation;
            return r;
        }
        r.program = ff_program(perm ? perm : ff_inputs_for_layout(h->layout));
        if (!perm) {  // the table's vertex shader is authoritative for the effect's default inputs
            if (const ShaderBlob* vs = find_shader_blob(h->vs)) r.program.vs = vs;
        }
        r.program.vertex_layout = h->layout;
        r.status = ProgramStatus::Ok;
        return r;
    }
    if (perm & ~static_cast<uint32_t>(kPermSkinned)) {
        r.status = ProgramStatus::UnknownPermutation;
        return r;
    }
    std::string vs = h->vs;
    VertexLayout layout = h->layout;
    if (perm & kPermSkinned) {
        if (vs.size() < 5 || vs.compare(vs.size() - 5, 5, "_skin") != 0) vs += "_skin";
        if (layout == VertexLayout::NU2) layout = VertexLayout::RSkinNU2;
        else if (layout == VertexLayout::NU2U3U3) layout = VertexLayout::RSkinNU2U3U3;
        else if (layout == VertexLayout::N) layout = VertexLayout::RSkinNU2;
    }
    r.program.vs = find_shader_blob(vs);
    r.program.ps = find_shader_blob(h->ps);
    if (!r.program.vs || !r.program.ps) {
        r.status = ProgramStatus::UnknownPermutation;
        r.program = Program{};
        return r;
    }
    r.program.iface = iface_for(h->family);
    r.program.family = h->family;
    r.program.vertex_layout = layout;
    r.program.permutation = perm;
    r.status = ProgramStatus::Ok;
    return r;
}

}  // namespace alamo::materials
