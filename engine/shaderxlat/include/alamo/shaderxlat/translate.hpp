// D3D9 SM1-2 program -> SM6 HLSL (entry VSMain / PSMain) for DXC -spirv, following the interface
// convention of docs/shaders/PORTING.md section 1 ("translated SM1-3 code"):
//
//   set 0 binding 0   cbuffer VSConstants { float4 c[256]; }      D3D9 vertex float constants
//   set 0 binding 1   cbuffer PSConstants { float4 c[32];  }      ps_2_0: c0..c31; ps_1_x: c0..c7 plus
//                     c16+n = bump-env matrix of stage n (M00, M01, M10, M11) and
//                     c24+n = (lscale, loffset, 0, 0) of stage n (texbem/texbeml/bem only)
//   set 0 binding 2   cbuffer VSIntConstants { int4 i[16]; uint4 b[4]; }  only when the VS reads i#/b#
//   set 0 binding 3   cbuffer PSIntConstants { int4 i[16]; uint4 b[4]; }  only when the PS reads i#/b#
//                     (b register n = b[n / 4][n % 4] != 0; def/defi/defb literals are inlined)
//   set 1 binding n   combined image sampler s_n (Texture2D / TextureCube / Texture3D + SamplerState
//                     pair with [[vk::combinedImageSampler]]); ps_1_x: sampler n = texture stage n
//   push constant     TranslatedPush (64 bytes, VS + PS), see below
//
// Vertex inputs: one float4 per dcl, at the fixed location of its (usage, index) (vertex_location()),
// compatible with the hand-written shaders' VertexLocation (POSITION 0, NORMAL 1, TEXCOORD0 2,
// TANGENT 3, BINORMAL 4, COLOR0 5). Missing components come from the vertex fetch (0,0,0,1).
// Varyings (VS out / PS in), matched by D3D semantic so any VS/PS pair links:
//   location 0 COLOR0, 1 COLOR1, 2..9 TEXCOORD0..7 (float4), 10 FOG (float).
// Every translated VS writes all 11 (unwritten ones get defaults: COLOR0 = 1, COLOR1 = 0,
// TEXCOORDn = (0,0,0,1), FOG = 1); a translated PS reads only those it uses plus FOG.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/shaderxlat/bytecode.hpp"

namespace alamo::shaderxlat {

// Bump to invalidate every cached translation when the emitted HLSL changes. (The cache key also
// hashes the emitted HLSL and the DXC arguments, see CacheKey::make.)
// v2: stricter ps_1_x operand validation, bump_env only for texbem/texbeml/bem, escaped sampler
//     names in the interface text, source-hashed cache keys.
constexpr uint32_t kTranslatorVersion = 2;

// Translation parameters that are not in the bytecode (the "interface key" of the cache). Defaults
// reproduce what the bytecode + CTAB say.
struct TranslateOptions {
    // Texture dimension per sampler slot. Unknown = dcl_* (SM2) or CTAB sampler type (ps_1_x), else 2D.
    std::array<TextureDim, 16> sampler_dims{};
    // ps_1_1..1_3: stages whose texture coordinates are projected (D3DTTFF_PROJECTED): tex divides by w.
    uint16_t projected_stages = 0;
    // Q3 instanced form (renderer mesh passes): the float constants are read from a StructuredBuffer<float4> at set 0
    // binding 0 (VS) / 1 (PS) instead of the cbuffers, at register (base + c#), base = instance x 256 (VS) / x 32 (PS)
    // when push Flags has kPushInstancedVs / kPushInstancedPs, else 0 (one shared image). The VS passes SV_InstanceID to
    // the PS as a flat varying (location kVaryingInstance). Same arithmetic as the cbuffer form; int / bool constants
    // stay cbuffers. Part of key() only when set (the default form's keys / program ids are unchanged).
    bool instanced = false;
    uint64_t key() const;   // stable 64-bit key of every field
};

// Push-constant block shared by every translated VS and PS (64 bytes).
struct TranslatedPush {
    uint32_t draw_index = 0;      // unused by translated code; keeps the layout of materials PushConstants
    uint32_t alpha_func = 7;      // VkCompareOp order (0 never .. 7 always = alpha test off); 8-bit alpha vs alpha_ref
    uint32_t alpha_ref = 0;       // 0..255
    uint32_t flags = 0;           // kPushFog | table-fog mode << 1
    float fog_color[4] = {0, 0, 0, 0};    // D3DRS_FOGCOLOR (rgb)
    float fog_params[4] = {0, 1, 1, 0};   // table fog: x start, y end, z density
    float pixel_offset[4] = {0, 0, 0, 0}; // xy: added to oPos.xy * oPos.w (D3D9 half-pixel: (1/W, -1/H) with a
                                          // negative-height viewport); zw unused
};
static_assert(sizeof(TranslatedPush) == 64, "push block");
constexpr uint32_t kPushFog = 1u;               // apply fog in the PS epilogue
constexpr uint32_t kFogTableShift = 1;          // 0 = vertex fog (oFog), 1 EXP, 2 EXP2, 3 LINEAR (from SV_Position.z)
constexpr uint32_t kPushInstancedVs = 1u << 8;  // instanced form: VS image per instance (stride 256 float4), else shared
constexpr uint32_t kPushInstancedPs = 1u << 9;  // instanced form: PS image per instance (stride 32 float4), else shared
constexpr uint32_t kTranslatedPushBytes = sizeof(TranslatedPush);

// Fixed vertex-input location for a D3D9 declaration (usage, index); 0xFFFFFFFF if it needs an
// overflow slot (assigned >= 16 in declaration order).
uint32_t vertex_location(Usage usage, uint32_t usage_index);
constexpr uint32_t kVaryingColor0 = 0, kVaryingColor1 = 1, kVaryingTexcoord0 = 2, kVaryingFog = 10, kVaryingCount = 11;
constexpr uint32_t kVaryingInstance = 11;  // instanced form only: flat uint instance index (VS -> PS)

struct VertexInputDesc {
    uint32_t location = 0;
    Usage usage = Usage::Position;
    uint8_t usage_index = 0;
    uint8_t reg = 0;              // v# in the bytecode
    uint8_t mask = 0xF;           // dcl write mask (components the shader declares)
    uint8_t components = 4;       // popcount of mask (the shader-side type is always float4)
};

enum class VaryingSemantic : uint8_t { Color, Texcoord, Fog };
struct VaryingDesc {
    uint32_t location = 0;
    VaryingSemantic semantic = VaryingSemantic::Texcoord;
    uint8_t index = 0;
    uint8_t mask = 0xF;           // VS: components the bytecode writes; PS: components it reads
};

struct SamplerDesc {
    uint8_t slot = 0;             // set 1 binding / s# / texture stage
    TextureDim dim = TextureDim::Tex2D;
    std::string name;             // CTAB sampler name ("" if unknown)
};

struct ShaderInterface {
    ShaderType stage = ShaderType::Vertex;
    uint8_t major = 0, minor = 0;
    std::vector<VertexInputDesc> inputs;   // VS only, sorted by location
    std::vector<VaryingDesc> varyings;     // VS: written outputs; PS: read inputs (sorted by location)
    std::vector<SamplerDesc> samplers;     // sorted by slot
    uint32_t float_constants = 0;          // highest c# read + 1 (def registers excluded; 256 if relative)
    bool relative_addressing = false;      // c[a0 + n] / c[aL + n] (then the whole file must be valid)
    bool int_bool_constants = false;       // needs set 0 binding 2 (VS) / 3 (PS)
    bool bump_env = false;                 // ps_1_x PS uses texbem/texbeml/bem: reads c16+n / c24+n bump env
    bool writes_depth = false;             // PS writes SV_Depth (oDepth / texdepth / texm3x2depth)
    bool can_discard = false;              // texkill (alpha test discards too, controlled by push)
    bool writes_point_size = false;        // VS writes oPts (PointSize builtin)
    uint32_t color_outputs = 1;            // PS: SV_Target0..n-1
    uint32_t push_constant_bytes = kTranslatedPushBytes;
    bool instanced = false;                // TranslateOptions::instanced form (storage-buffer constants, binding 0 / 1)
    std::string profile;                   // "vs_1_1" ...

    // Line-based text form ("key value" per line), stable across versions of the same
    // kTranslatorVersion. Used by the disk cache and the offline tool.
    std::string serialize() const;
    static bool deserialize(std::string_view text, ShaderInterface& out);
    std::string to_json() const;
};

struct Translation {
    std::string hlsl;
    std::string entry;            // "VSMain" / "PSMain"
    std::string target;           // "vs_6_0" / "ps_6_0"
    ShaderInterface iface;
};

// Deterministic: the same program + options give the same text on every run and thread.
// Fails (false + *err) for SM3 or opcodes invalid for the shader type.
bool translate(const Program& program, const TranslateOptions& options, Translation& out, std::string* err = nullptr);

// Effective texture dimension of a sampler slot (options > dcl > CTAB > 2D). Used by the
// translator and the interpreter alike.
TextureDim sampler_dim(const Program& program, const TranslateOptions& options, uint32_t slot);

// DXC arguments the engine uses for translated code (also used by the offline tool).
// -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -O3 -WX -HV 2021
std::vector<std::string> dxc_arguments(const Translation& t);

}  // namespace alamo::shaderxlat
